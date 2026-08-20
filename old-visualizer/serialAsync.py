import asyncio
import fcntl
import struct
import logging

log = logging.getLogger(__name__)

# TI mmWave standard frame magic word
MAGIC_WORD = b'\x02\x01\x04\x03\x06\x05\x08\x07'
# Header layout after magic word (all little-endian uint32):
#   version, totalPacketLen, platform, frameNumber, timeCpuCycles,
#   numDetectedObj, numTLVs, subFrameNumber
HEADER_LEN = 40           # 8 (magic word) + 8*4 bytes
TOTAL_LEN_OFFSET = 12     # offset of totalPacketLen field within the frame
FRAME_NUMBER_OFFSET = 20  # offset of frameNumber field within the frame

# --- Hardware-level loss detection --------------------------------------
# TIOCGICOUNT gives kernel-tracked counters directly from the tty driver,
# including buf_overrun -- bytes the driver had to discard because
# userspace wasn't reading fast enough. This is ground truth: if
# buf_overrun stays at 0, no bytes were physically lost between the UART
# and your process, and any missing frames are a parsing/framing bug, not
# a throughput limit.
_TIOCGICOUNT = 0x545D
_ICOUNT_STRUCT = struct.Struct('11i')  # cts,dsr,rng,dcd,rx,tx,frame,overrun,parity,brk,buf_overrun


def get_overrun_count(fd: int) -> dict:
    """Returns kernel-level rx/frame/overrun/buf_overrun counters for an
    open serial file descriptor. Call periodically and diff against the
    previous reading."""
    raw = fcntl.ioctl(fd, _TIOCGICOUNT, bytes(_ICOUNT_STRUCT.size))
    cts, dsr, rng, dcd, rx, tx, frame, overrun, parity, brk, buf_overrun = _ICOUNT_STRUCT.unpack(raw)
    return {
        'rx': rx, 'frame_errors': frame, 'overrun': overrun,
        'parity': parity, 'break_count': brk, 'buf_overrun': buf_overrun,
    }


def handle_line(line: str):
    print(f"LINE: {line!r}")


class CliSerialCore(asyncio.Protocol):
    """Line-oriented protocol for the CLI/config COM port (text commands)."""

    def __init__(self, on_line):
        self.on_line = on_line
        self.buf = bytearray()
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        log.info("CLI serial port opened")

    def connection_lost(self, exc):
        # flush any remaining partial line, then signal shutdown
        if self.buf:
            text = self.buf.replace(b'\r', b'').decode(errors='replace')
            if text:
                self.on_line(text)
        if exc:
            log.error(f"CLI serial connection closed with error: {exc}")
        else:
            log.info("CLI serial port closed")

    def data_received(self, data: bytes):
        self.buf.extend(data)
        try:
            while True:
                idx = self.buf.find(b'\n')
                if idx == -1:
                    break
                line = bytes(self.buf[:idx])
                # delete in place instead of rebuilding the bytearray
                del self.buf[:idx + 1]
                text = line.replace(b'\r', b'').decode(errors='replace')
                if text:
                    self.on_line(text)
        except Exception as e:
            log.exception(f'{e}')

    def write_data(self, data: str):
        self.transport.write(data.encode())


class DataSerialCore(asyncio.Protocol):
    """
    Binary TLV protocol for the high-rate data COM port.

    data_received() ONLY extracts complete frames and pushes them onto an
    asyncio.Queue. Actual parsing (parseStandardFrame, TLV decoding, GUI
    updates, etc.) happens in a separate consumer task so the reader is
    never blocked and the kernel tty buffer never has time to overflow.
    """

    def __init__(self, frame_queue: asyncio.Queue):
        self.frame_queue = frame_queue
        self.buf = bytearray()
        self.transport = None

        # --- loss-detection counters ---
        self.last_frame_number = None
        self.lost_frames = 0        # gaps detected via frameNumber
        self.resync_events = 0      # times we had to hunt for magic word
        self.garbage_bytes = 0      # bytes discarded while resyncing
        self.queue_drops = 0        # frames dropped because consumer was slow

    def connection_made(self, transport):
        self.transport = transport
        log.info("Data serial port opened")

    def stats(self) -> dict:
        return {
            'lost_frames': self.lost_frames,
            'resync_events': self.resync_events,
            'garbage_bytes': self.garbage_bytes,
            'queue_drops': self.queue_drops,
        }

    def connection_lost(self, exc):
        if exc:
            log.error(f"Data serial connection closed with error: {exc}")
        else:
            log.info("Data serial port closed")

    def data_received(self, data: bytes):
        self.buf.extend(data)
        while True:
            frame = self._try_extract_frame()
            if frame is None:
                break
            try:
                self.frame_queue.put_nowait(frame)
            except asyncio.QueueFull:
                # Consumer can't keep up -- drop oldest rather than block
                # the reader (blocking here is what causes overflow).
                self.queue_drops += 1
                log.warning(f"Frame queue full, dropping oldest frame (total drops: {self.queue_drops})")
                self.frame_queue.get_nowait()
                self.frame_queue.put_nowait(frame)

    def _try_extract_frame(self):
        start = self.buf.find(MAGIC_WORD)
        if start == -1:
            # No magic word yet. Keep at most the last (len(MAGIC_WORD)-1)
            # bytes in case the word is split across two reads.
            if len(self.buf) > len(MAGIC_WORD):
                del self.buf[:len(self.buf) - len(MAGIC_WORD) + 1]
            return None

        if start > 0:
            # Drop garbage before the magic word (e.g. resync after a drop).
            # This is the clearest protocol-level symptom of lost/corrupted
            # bytes: a clean stream should always have the magic word at
            # offset 0 once the previous frame was fully consumed.
            self.resync_events += 1
            self.garbage_bytes += start
            log.warning(
                f"Resync: dropped {start} garbage byte(s) before magic word "
                f"(resync #{self.resync_events}, total garbage {self.garbage_bytes})"
            )
            del self.buf[:start]

        if len(self.buf) < HEADER_LEN:
            return None  # header not fully arrived yet

        total_len = struct.unpack_from('<I', self.buf, TOTAL_LEN_OFFSET)[0]

        if total_len < HEADER_LEN or total_len > 10_000_000:
            # Corrupt/garbage length field -- resync past this magic word
            self.resync_events += 1
            del self.buf[:len(MAGIC_WORD)]
            return None

        if len(self.buf) < total_len:
            return None  # full frame hasn't arrived yet

        frame = bytes(self.buf[:total_len])
        del self.buf[:total_len]

        frame_number = struct.unpack_from('<I', frame, FRAME_NUMBER_OFFSET)[0]
        if self.last_frame_number is not None:
            gap = frame_number - self.last_frame_number - 1
            if gap > 0:
                self.lost_frames += gap
                log.warning(
                    f"Detected {gap} missing frame(s): expected "
                    f"{self.last_frame_number + 1}, got {frame_number} "
                    f"(total lost: {self.lost_frames})"
                )
            elif gap < 0:
                log.warning(
                    f"frameNumber went backwards ({self.last_frame_number} -> "
                    f"{frame_number}) -- sensor likely restarted"
                )
        self.last_frame_number = frame_number

        return frame

