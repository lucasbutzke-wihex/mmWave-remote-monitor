import sys
import socket
import struct
import numpy as np
import requests

from PyQt5 import QtCore, QtWidgets
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

# Update this to match your Raspberry Pi's local network IP address
SERVER_IP = "100.109.224.24"
SERVER_PORT = 5001
MAGIC_WORD = b'\x02\x01\x04\x03\x06\x05\x08\x07'

NUM_RANGE_BINS = 512
NUM_DOPPLER_BINS = 16

PKT_TYPE_CLI_RESP = 1
PKT_TYPE_RADAR = 2
PKT_TYPE_SYSTEM = 99

class RadarNetworkWorker(QtCore.QThread):
    range_profile_signal = QtCore.pyqtSignal(np.ndarray)
    heatmap_signal = QtCore.pyqtSignal(np.ndarray)

    HEADER_STRUCT = struct.Struct('!III')

    def __init__(self):
        super().__init__()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(1.0)
        self.running = True
        self.net_buffer = bytearray()
        self.data_buffer = bytearray()

    def run(self):
        try:
            self.sock.connect((SERVER_IP, SERVER_PORT))
        except OSError as e:
            print(f"[TCP] Connection failed: {e}")
            return

        print(f"[TCP] Connected to {SERVER_IP}:{SERVER_PORT}")

        try:
            self.sock.sendall(b"RESET\n")
        except OSError as e:
            print(f"[TCP] Failed to send initial RESET: {e}")
            return

        while self.running:
            try:
                chunk = self.sock.recv(8192)
                if not chunk:
                    print("[TCP] Server closed the connection.")
                    break
                self.net_buffer.extend(chunk)
                self.process_net_buffer()

            except socket.timeout:
                try:
                    self.sock.sendall(b"\n")
                except OSError as e:
                    print(f"[TCP] Keepalive send failed: {e}")
                    break

            except Exception as e:
                print(f"[TCP] Network error: {e}")
                break

        self.sock.close()

    def process_net_buffer(self):
        offset = 0
        n = len(self.net_buffer)

        while True:
            if n - offset < self.HEADER_STRUCT.size:
                break

            p_type, _seq, p_len = self.HEADER_STRUCT.unpack_from(self.net_buffer, offset)
            total_len = self.HEADER_STRUCT.size + p_len

            if n - offset < total_len:
                break

            payload_start = offset + self.HEADER_STRUCT.size
            payload = bytes(self.net_buffer[payload_start: payload_start + p_len])

            self.handle_message(p_type, payload)
            offset += total_len

        if offset:
            del self.net_buffer[:offset]

    def handle_message(self, p_type, payload):
        if p_type == PKT_TYPE_SYSTEM:
            self.data_buffer = bytearray()
        elif p_type == PKT_TYPE_RADAR:
            self.data_buffer.extend(payload)
            self.parse_buffer()
        elif p_type == PKT_TYPE_CLI_RESP:
            try:
                print(f"[CLI] {payload.decode(errors='replace')}")
            except Exception:
                pass

    def parse_buffer(self):
        offset = 0
        n = len(self.data_buffer)

        while n - offset >= 48:
            magic_idx = self.data_buffer.find(MAGIC_WORD, offset)
            if magic_idx == -1:
                if n - offset > 32:
                    offset = n - 7
                break

            if n - magic_idx < 16:
                break

            total_packet_len = struct.unpack_from('<I', self.data_buffer, magic_idx + 12)[0]
            if n - magic_idx < total_packet_len:
                break

            frame_bytes = bytes(self.data_buffer[magic_idx: magic_idx + total_packet_len])
            self.extract_tlvs(frame_bytes)

            offset = magic_idx + total_packet_len

        if offset:
            del self.data_buffer[:offset]

    def extract_tlvs(self, frame_bytes):
        header_format = '<QIIIIIIII'
        h_size = struct.calcsize(header_format)
        if len(frame_bytes) < h_size:
            return

        header = struct.unpack(header_format, frame_bytes[:h_size])
        num_tlvs = header[7]

        pointer = h_size
        for _ in range(num_tlvs):
            if pointer + 8 > len(frame_bytes):
                break
            tlv_type, tlv_len = struct.unpack('<II', frame_bytes[pointer: pointer + 8])
            data_ptr = pointer + 8

            payload = frame_bytes[data_ptr: data_ptr + tlv_len]

            print(f"Extracted TLV Type: {tlv_type}, Length: {tlv_len}, "
                  f"Payload bytes actually available: {len(payload)}")

            if tlv_type == 2:
                arr = np.frombuffer(payload, dtype=np.uint16)
                self.range_profile_signal.emit(arr.copy())

            elif tlv_type == 5:
                expected_elements = NUM_RANGE_BINS * NUM_DOPPLER_BINS
                expected_bytes = expected_elements * 2

                if len(payload) != expected_bytes:
                    print(f"  [WARN] Heatmap TLV size mismatch: got {len(payload)} bytes, "
                          f"expected {expected_bytes} bytes (tlv_len={tlv_len}).")

                truncated_payload = payload[:expected_bytes]
                matrix_flat = np.frombuffer(truncated_payload, dtype=np.uint16)

                if len(matrix_flat) < expected_elements:
                    padded_array = np.zeros(expected_elements, dtype=np.uint16)
                    padded_array[:len(matrix_flat)] = matrix_flat
                    matrix_flat = padded_array

                matrix_2d = matrix_flat.reshape((NUM_RANGE_BINS, NUM_DOPPLER_BINS))
                shifted_matrix = np.fft.fftshift(matrix_2d, axes=1)
                log_matrix = np.log10(shifted_matrix + 1)

                self.heatmap_signal.emit(log_matrix.copy())

            # Move pointer forward past current header + payload length
            pointer += 8 + tlv_len
            
            # TLV ALIGNMENT FIX: Handle structural 4-byte padding matching the mmWave SDK specification
            if pointer % 4 != 0:
                pointer += (4 - (pointer % 4))

    def stop(self):
        self.running = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.wait()


class RangeProfileCanvas(FigureCanvas):
    def __init__(self, parent=None):
        self.fig = Figure(figsize=(6, 4))
        super().__init__(self.fig)
        self.setParent(parent)
        self.ax = self.fig.add_subplot(111)
        self.line, = self.ax.plot([])
        self.ax.set_title("Range Profile")
        self.ax.set_xlabel("Distance (m)")
        self.ax.set_ylabel("Amplitude")
        self.ax.grid(True)

    def update_data(self, range_profile, dist_bins):
        self.line.set_data(np.arange(len(range_profile))*dist_bins, range_profile)
        
        self.ax.relim()
        self.ax.autoscale_view()
        self.draw()


class HeatmapCanvas(FigureCanvas):
    def __init__(self, parent=None):
        self.fig = Figure(figsize=(6, 4))
        super().__init__(self.fig)
        self.setParent(parent)
        self.ax = self.fig.add_subplot(111)
        self.colorbar = None
        self.im = None

    def update_data(self, log_matrix, range_resolution, num_doppler_bins=NUM_DOPPLER_BINS):
        num_range_bins = log_matrix.shape[0]
        max_distance_meters = num_range_bins * range_resolution / 2

        extent = [
            -num_doppler_bins // 2, 
            num_doppler_bins // 2 - 1,
            0, 
            max_distance_meters
        ]
        if self.im is None:

            self.im = self.ax.imshow(log_matrix, cmap='jet', aspect='auto',
                                      extent=extent, origin='lower')
            self.colorbar = self.fig.colorbar(self.im, ax=self.ax, label='Log Energy / Intensity')
            self.ax.set_title('TI mmWave Range-Doppler Heatmap')
            self.ax.set_xlabel(r'Doppler Bin Index ($\leftarrow$ Away | Toward $\rightarrow$)')
            self.ax.set_ylabel('Distance (m)')
            self.ax.grid(color='w', linestyle='--', alpha=0.5)
        else:
            self.im.set_data(log_matrix)
            self.im.set_clim(vmin=float(log_matrix.min()), vmax=float(log_matrix.max()))

        self.draw_idle()


class RadarMainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("TI mmWave Radar Live Plotter (TCP, matplotlib)")
        self.resize(1300, 700)

        central_widget = QtWidgets.QWidget()
        self.setCentralWidget(central_widget)
        layout = QtWidgets.QHBoxLayout(central_widget)

        self.range_canvas = RangeProfileCanvas(self)
        self.heatmap_canvas = HeatmapCanvas(self)
        layout.addWidget(self.range_canvas)
        layout.addWidget(self.heatmap_canvas)

        self.thread = RadarNetworkWorker()
        self.thread.range_profile_signal.connect(self.update_line_plot)
        self.thread.heatmap_signal.connect(self.update_heatmap_plot)
        self.thread.start()

        config_file = self.read_cfg_mmwave_web("http://100.109.224.24:8001/config.cfg")
        self.distance_bins = self.convert_range_bin_meters(config_file)

    @QtCore.pyqtSlot(np.ndarray)
    def update_line_plot(self, data):
        self.range_canvas.update_data(data, self.distance_bins)

    @QtCore.pyqtSlot(np.ndarray)
    def update_heatmap_plot(self, log_matrix):
        min_val = float(np.min(log_matrix))
        max_val = float(np.max(log_matrix))
        print(f"[DEBUG GRAPH UPDATE] Processing bounds: Min={min_val:.2f}, Max={max_val:.2f}")
        self.heatmap_canvas.update_data(log_matrix.astype(np.float32), self.distance_bins)

    def closeEvent(self, event):
        self.thread.stop()
        event.accept()

    def read_cfg_mmwave_web(self, url: str) -> dict:
        """baixa o .cfg da web e extrai os parâmetros. """

        response = requests.get(url)
        response.raise_for_status()

        config = {}

        lines = response.text.splitlines() #processa conteudo

        for line in lines:
            empty_line = line.strip()

            if not empty_line or empty_line.startswith("%"): #ignora linhas vazias e comentadas
                continue

            # parse
            parts = empty_line.split()
            command = parts[0]
            arguments = parts[1:]

            # Converte valores para int/float
            converted_arg = []
            for arg in arguments:
                try:
                    if "." in arg:
                        converted_arg.append(float(arg))
                    else:
                        converted_arg.append(int(arg))
                except ValueError:
                    converted_arg.append(
                        arg
                    )

            config[command] = converted_arg

        return config

    def convert_range_bin_meters(self, dictionary: dict) -> float: # retorna em metros/bin, deve-se multiplicar pelo bin para ter a distancia
        profileCfg = dictionary["profileCfg"]
        slope = profileCfg[7] * 10**12; # Multiplica por 10¹² para converter para Hz/s ao inves de MHz/us
        numAdcSamples = profileCfg[9]
        freq = profileCfg[10] * 10**3 # multiplica por 10³ para converter de kpbs para Hz

        range_bin_resol_meters = (3*(10**8)*freq)/(2*slope*numAdcSamples)

        return range_bin_resol_meters


if __name__ == '__main__':
    app = QtWidgets.QApplication(sys.argv)
    window = RadarMainWindow()
    window.show()
    sys.exit(app.exec_())
