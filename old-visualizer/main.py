import asyncio
import sys
import os
import logging

import serial_asyncio_fast

from serialAsync import CliSerialCore, DataSerialCore, handle_line, get_overrun_count
from parseFrame import parseStandardFrame

logging.basicConfig(
    format='%(asctime)s,%(msecs)03d %(levelname)-8s [%(filename)s:%(lineno)d] %(message)s',
    datefmt='%Y-%m-%d:%H:%M:%S',
    level=logging.INFO,
)
log = logging.getLogger(__name__)

sys.path.insert(1, os.path.join(os.path.abspath(os.getcwd()), "common"))
sys.path.insert(1, '../common')

# Confirm this against your device config (.cfg) baudRate command --
# 921600 is the TI mmWave demo default for the data port; adjust if your
# adapter/setup is actually configured for a different rate (e.g. 1000000).
DATA_PORT_BAUDRATE = 921600


async def frame_consumer(frame_queue: asyncio.Queue):
    """Runs independently of the serial reader -- parsing/GUI work here
    never blocks incoming data."""
    while True:
        frame = await frame_queue.get()
        try:
            parseStandardFrame(frame)
        except Exception as e:
            log.exception(f"Error parsing frame: {e}")


async def loss_monitor(data_protocol: DataSerialCore, fd: int, interval: float = 5.0):
    """Periodically logs protocol-level (frameNumber gaps, resyncs) and
    hardware-level (kernel buf_overrun) loss indicators.

    buf_overrun > 0  -> bytes were physically lost between UART and process
                         (real throughput/latency_timer/scheduling problem).
    lost_frames > 0 but buf_overrun == 0 -> bytes arrived fine but a bug in
                         framing/parsing is dropping or corrupting frames.
    """
    prev_overrun = 0
    while True:
        await asyncio.sleep(interval)
        s = data_protocol.stats()
        hw = get_overrun_count(fd)
        new_overrun = hw['buf_overrun'] - prev_overrun
        prev_overrun = hw['buf_overrun']
        log.info(
            f"[loss-monitor] lost_frames={s['lost_frames']} "
            f"resync_events={s['resync_events']} "
            f"garbage_bytes={s['garbage_bytes']} "
            f"queue_drops={s['queue_drops']} "
            f"kernel_buf_overrun_delta={new_overrun} "
            f"kernel_frame_errors={hw['frame_errors']} "
            f"kernel_parity_errors={hw['parity']}"
        )


async def main():
    frame_queue: asyncio.Queue = asyncio.Queue(maxsize=64)

    try:
        cli_transport, _ = await serial_asyncio_fast.create_serial_connection(
            asyncio.get_running_loop(),
            lambda: CliSerialCore(handle_line),
            "/dev/ttyAMA2",
            baudrate=115200,
        )

        data_protocol_holder = {}

        def make_data_protocol():
            p = DataSerialCore(frame_queue)
            data_protocol_holder['protocol'] = p
            return p

        data_transport, _ = await serial_asyncio_fast.create_serial_connection(
            asyncio.get_running_loop(),
            make_data_protocol,
            "/dev/ttyAMA0",
            baudrate=DATA_PORT_BAUDRATE,
        )

        # serial_asyncio_fast transports expose the underlying fd for ioctl access
        data_fd = data_transport.serial.fileno()

        consumer_task = asyncio.create_task(frame_consumer(frame_queue))
        monitor_task = asyncio.create_task(
            loss_monitor(data_protocol_holder['protocol'], data_fd)
        )

        try:
            await asyncio.Future()  # run forever
        finally:
            consumer_task.cancel()
            monitor_task.cancel()
            cli_transport.close()
            data_transport.close()

    except Exception as e:
        log.exception(f"Exception: {e}")


if __name__ == '__main__':
    asyncio.run(main())

