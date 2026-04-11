# HLK-LD2410S mmWave presence sensor driver and debug tool.
#
# Manufacturer page: https://www.hlktech.net/index.php?id=1176
# Protocol: https://drive.google.com/file/d/1LFyf6w9nOxW7b5z0rg5I3mPkk2KjQviE/view
# Sensor manual: https://drive.google.com/file/d/1RYpSp6NaCerTm2P-qLDfyDEjUKo1RAYG/view
# Manufacturer docs: https://drive.google.com/drive/folders/1wC8KC-DaNavNbpeVouZ1HdiBzZ9YrAcg
# Another impl of a driver: https://registry.platformio.org/libraries/phuongnamzz/HLK-LD2410S
# Yet another impl: https://github.com/MrUndead1996/ld2410s-esphome/blob/main/components/ld2410s/LD2410S.cpp
#
# Connects to the sensor over USB-UART (115200 baud) and provides:
#   - Continuous occupancy/distance reports from the sensor
#   - Reading and writing all sensor configuration parameters
#   - Calibration support with progress tracking
#
# Architecture:
#   LD2410S_frameHandler  - layer 1: frame detection, send/receive, command queue
#   LD2410S               - layer 2: parses frames into typed callbacks
#
# When run directly, polls ./cmd for commands. Write a command to the file
# and the script will execute it and delete the file:
#
#   echo "all" > ./cmd          # read all config (fw, sn, params)
#   echo "sn" > ./cmd           # read serial number
#   echo "sn MYSERIAL" > ./cmd  # set serial number
#   echo "fw" > ./cmd           # read firmware version
#   echo "common" > ./cmd       # read common parameters
#   echo "threshold" > ./cmd    # read threshold parameters (gates 0-7)
#   echo "snr" > ./cmd          # read SNR parameters (gates 8-15)
#   echo "set farthest_gate 8" > ./cmd   # set a parameter
#   echo "calibrate" > ./cmd    # start calibration (default: trigger=2 retention=1 duration=120s)
#   echo "calibrate 2 1 60" > ./cmd      # calibration with custom params
#   echo "config" > ./cmd       # manually enable config mode
#   echo "end" > ./cmd          # manually end config mode
#   echo "raw 0000" > ./cmd     # send raw command word (hex)
#
# Config mode is automatically entered/exited for commands that need it.
# Pass --debug for raw frame logging. Device defaults to /dev/ttyUSB0.
#
#   python ld2410s.py [/dev/ttyUSBx] [--debug]

import os
import queue
import serial
import struct
import sys
import threading
import time

CMD_HEADER = b'\xfd\xfc\xfb\xfa'
CMD_FOOTER = b'\x04\x03\x02\x01'
CALIBRATION_REPORT_HEADER = b'\xf4\xf3\xf2\xf1'
CALIBRATION_REPORT_FOOTER = b'\xf8\xf7\xf6\xf5'

class LD2410S_frameHandler:
    """ Send and receive frames from an LD2410S sensor. Detects boundaries between frames and manages command
    queue to the sensor. Doesn't parse or interpret the frames. """

    _CMD_TIMEOUT = 2.0  # seconds to wait for a response before sending next

    def __init__(self, dev_path, debug=False):
        self.ser = serial.Serial(dev_path, 115200, timeout=0.1)
        self._cmd_queue = queue.Queue()
        self._waiting_for_response = False
        self._response_timeout = 0
        self._thread = None
        self._debug = debug

    def _run_main_loop(self):
        buf = bytearray()
        while True:
            self._try_send_next()

            b = self.ser.read(1)
            if not b:
                continue
            buf.append(b[0])

            if self._parse_buffer(buf):
                buf.clear()

            if len(buf) > 256:
                print(f"Err: Can't find command in buffer: {buf.hex(' ')}")
                buf.clear()

    def _try_send_next(self):
        if self._waiting_for_response:
            if time.monotonic() < self._response_timeout:
                return
            print("Err: timeout waiting for response, moving on")
            self._waiting_for_response = False

        try:
            command_word, data = self._cmd_queue.get_nowait()
            self.send_cmd(command_word, data)
            self._waiting_for_response = True
            self._response_timeout = time.monotonic() + self._CMD_TIMEOUT
        except queue.Empty:
            pass

    def _parse_buffer(self, buf):
        # Detect short report frames
        if len(buf) >= 5 and buf[-5] == 0x6E and buf[-1] == 0x62:
            self.on_maybe_desync_frame(buf[0:-5])
            frame = buf[-5:]
            if self._debug:
                print(f"[RX] report {frame.hex(' ')}")
            self.on_report_frame(frame)
            return True

        # Detect responses to commands
        if buf[-len(CMD_FOOTER):] == bytearray(CMD_FOOTER):
            i = buf.rfind(CMD_HEADER)
            if i < 0:
                self.on_maybe_desync_frame(buf)
            else:
                self.on_maybe_desync_frame(buf[0:i])
                self._waiting_for_response = False
                frame = buf[i:]
                if self._debug:
                    print(f"[RX] cmd_response {frame.hex(' ')}")
                self.on_command_response_frame(frame)
            return True

        # Detect calibration report frames (F4 F3 F2 F1 ... F8 F7 F6 F5)
        if buf[-len(CALIBRATION_REPORT_FOOTER):] == bytearray(CALIBRATION_REPORT_FOOTER):
            i = buf.rfind(CALIBRATION_REPORT_HEADER)
            if i < 0:
                self.on_maybe_desync_frame(buf)
            else:
                self.on_maybe_desync_frame(buf[0:i])
                frame = buf[i:]
                if self._debug:
                    print(f"[RX] calibration_progress {frame.hex(' ')}")
                self.on_calibration_progress_frame(frame)
            return True

        # Nothing parseable found
        return False

    def start(self):
        """Start the reader loop in a background thread. Returns immediately."""
        self._thread = threading.Thread(target=self._run_main_loop, daemon=True)
        self._thread.start()

    def send_cmd(self, command_word, data=b''):
        payload = struct.pack('<H', command_word) + data
        length = struct.pack('<H', len(payload))
        frame = CMD_HEADER + length + payload + CMD_FOOTER
        if self._debug:
            print(f"[TX] {frame.hex(' ')}")
        self.ser.write(frame)

    def enqueue_cmd(self, command_word, data=b''):
        """Enqueue a command to be sent from the reader thread."""
        self._cmd_queue.put((command_word, data))

    def on_maybe_desync_frame(self, partial_frame):
        if partial_frame and self._debug:
            print(f"[RX] partial frame: {partial_frame.hex(' ')}")

    def on_report_frame(self, frame):
        pass

    def on_command_response_frame(self, frame):
        pass

    def on_calibration_progress_frame(self, frame):
        pass



# Request command words
CMD_ENABLE_CONFIG   = 0x00FF
CMD_END_CONFIG      = 0x00FE
CMD_READ_FIRMWARE   = 0x0000
CMD_WRITE_SERIAL    = 0x0010
CMD_READ_SERIAL     = 0x0011
CMD_CALIBRATE       = 0x0009
CMD_WRITE_COMMON    = 0x0070
CMD_READ_COMMON     = 0x0071
CMD_WRITE_THRESHOLD = 0x0072
CMD_READ_THRESHOLD  = 0x0073
CMD_WRITE_SNR       = 0x0074
CMD_READ_SNR        = 0x0075


COMMON_PARAM_NAMES = [
    (0x05, "farthest_gate"),
    (0x0A, "nearest_gate"),
    (0x06, "unmanned_delay"),
    (0x02, "status_report_freq"),
    (0x0C, "dist_report_freq"),
    (0x0B, "response_speed"),
]

ALL_PARAMS = {
    "farthest_gate":      (CMD_WRITE_COMMON, 0x05),
    "nearest_gate":       (CMD_WRITE_COMMON, 0x0A),
    "unmanned_delay":     (CMD_WRITE_COMMON, 0x06),
    "status_report_freq": (CMD_WRITE_COMMON, 0x02),
    "dist_report_freq":   (CMD_WRITE_COMMON, 0x0C),
    "response_speed":     (CMD_WRITE_COMMON, 0x0B),
}
for _i in range(8):
    ALL_PARAMS[f"gate_{_i}_trigger"] = (CMD_WRITE_THRESHOLD, _i)
    ALL_PARAMS[f"gate_{_i}_holding"] = (CMD_WRITE_THRESHOLD, _i + 8)
for _i in range(8):
    ALL_PARAMS[f"gate_{_i+8}_trigger_snr"] = (CMD_WRITE_SNR, _i)
    ALL_PARAMS[f"gate_{_i+8}_hold_snr"] = (CMD_WRITE_SNR, _i + 8)
ALL_PARAMS["serial_number"] = (CMD_WRITE_SERIAL, None)

class LD2410S(LD2410S_frameHandler):
    def on_report_frame(self, frame):
        state = frame[1]
        dist = frame[2] | (frame[3] << 8)
        occupancy = not (state == 0 or state == 1)
        self.on_sensor_report(occupancy, dist)

    def on_calibration_progress_frame(self, frame):
        payload_len = struct.unpack('<H', frame[len(CALIBRATION_REPORT_HEADER):len(CALIBRATION_REPORT_HEADER) + 2])[0]
        payload = frame[len(CALIBRATION_REPORT_HEADER) + 2:-len(CALIBRATION_REPORT_HEADER)]
        if len(payload) != payload_len:
            print(f"[RX] Bad calibration report frame received len={len(payload)} expected_len={payload_len}")
        if len(payload) >= 3:
            self.on_calibration_progress(payload[1] | (payload[2] << 8))

    def on_command_response_frame(self, frame):
        payload_len = struct.unpack('<H', frame[len(CMD_HEADER):len(CMD_HEADER) + 2])[0]
        payload = frame[len(CMD_HEADER) + 2:-len(CMD_FOOTER)]

        if len(payload) != payload_len:
            print(f"[RX] Error: Bad command response, len={len(payload)} expected_len={payload_len}")
            # Fall through, attempt to parse anyway

        if len(payload) < 4:
            print(f"[RX] Error: Bad command response, short payload: {payload.hex(' ')}")
            return

        resp_cmd = struct.unpack('<H', payload[0:2])[0]
        status = struct.unpack('<H', payload[2:4])[0]
        data = payload[4:]

        if status != 0:
            self.on_command_error(resp_cmd, status)
            return

        # resp_cmd will be the same as the command ID + 0x100
        R = 0x0100  # response offset
        response_handlers = {
            CMD_ENABLE_CONFIG  + R: self.on_enable_config,
            CMD_END_CONFIG     + R: self.on_end_config,
            CMD_READ_FIRMWARE  + R: self.on_firmware_version,
            CMD_READ_SERIAL    + R: self.on_serial_number_read,
            CMD_WRITE_SERIAL   + R: self.on_serial_number_write,
            CMD_CALIBRATE      + R: self.on_calibrate,
            CMD_READ_COMMON    + R: self.on_common_params,
            CMD_READ_THRESHOLD + R: self.on_threshold_params,
            CMD_READ_SNR       + R: self.on_snr_params,
        }
        write_ack_cmds = {
            CMD_WRITE_COMMON    + R,
            CMD_WRITE_THRESHOLD + R,
            CMD_WRITE_SNR       + R,
        }

        if response_handlers.get(resp_cmd):
            response_handlers.get(resp_cmd)(data)
        elif resp_cmd in write_ack_cmds:
            self.on_param_write(resp_cmd, data)
        else:
            self.on_unknown_command(resp_cmd, data)


    def on_sensor_report(self, occupancy, distance):
        print(f"Report: Occupancy={occupancy} Distance={distance}")

    def on_calibration_progress(self, progress):
        print(f"Calibration progress: {progress}%")

    def on_command_error(self, resp_cmd, status):
        print(f"[CMD] error: resp=0x{resp_cmd:04x} status=0x{status:04x}")

    def on_unknown_command(self, resp_cmd, data):
        print(f"[CMD] unknown resp=0x{resp_cmd:04x} data={data.hex(' ')}")

    def on_enable_config(self, data):
        print(f"[CMD] config mode enabled")

    def on_end_config(self, data):
        print(f"[CMD] config mode ended")

    def on_firmware_version(self, data):
        print(f"[CMD] firmware: {data.hex(' ')}")

    def on_serial_number_read(self, data):
        sn_len = struct.unpack('<H', data[0:2])[0]
        sn = data[2:2 + sn_len].decode('ascii', errors='replace').rstrip('\x00')
        print(f"[CMD] serial number: {sn}")

    def on_serial_number_write(self, data):
        print(f"[CMD] serial number set OK")

    def on_common_params(self, data):
        print(f"[CMD] common parameters:")
        for i, (_, name) in enumerate(COMMON_PARAM_NAMES):
            off = i * 4
            if off + 4 <= len(data):
                val = struct.unpack('<I', data[off:off + 4])[0]
                print(f"  {name}: {val}")

    def on_threshold_params(self, data):
        print(f"[CMD] threshold parameters (gates 0-7):")
        print(f"  {'Gate':<6} {'Trigger':>8} {'Holding':>8}")
        for i in range(8):
            if (i + 8) * 4 + 4 <= len(data):
                trigger = struct.unpack('<I', data[i * 4:i * 4 + 4])[0]
                holding = struct.unpack('<I', data[(i + 8) * 4:(i + 8) * 4 + 4])[0]
                print(f"  {i:<6} {trigger:>8} {holding:>8}")

    def on_snr_params(self, data):
        print(f"[CMD] SNR parameters (gates 8-15):")
        print(f"  {'Gate':<6} {'Trigger':>8} {'Hold':>8}")
        for i in range(8):
            if (i + 8) * 4 + 4 <= len(data):
                trigger = struct.unpack('<I', data[i * 4:i * 4 + 4])[0]
                hold = struct.unpack('<I', data[(i + 8) * 4:(i + 8) * 4 + 4])[0]
                print(f"  {i + 8:<6} {trigger:>8} {hold:>8}")

    def on_calibrate(self, data):
        print(f"[CMD] calibration command accepted")

    def on_param_write(self, resp_cmd, data):
        print(f"[CMD] param write OK (resp=0x{resp_cmd:04x})")



CMD_FILE = "./cmd"

def _config_wrap(sensor, *cmds):
    """Wrap commands with config enable/end."""
    sensor.enqueue_cmd(CMD_ENABLE_CONFIG, struct.pack('<H', 0x0001))
    for command_word, data in cmds:
        sensor.enqueue_cmd(command_word, data)
    sensor.enqueue_cmd(CMD_END_CONFIG)


def process_command(sensor, line):
    parts = line.split()
    if not parts:
        return
    cmd = parts[0].lower()

    if cmd == "config":
        sensor.enqueue_cmd(CMD_ENABLE_CONFIG, struct.pack('<H', 0x0001))
    elif cmd == "end":
        sensor.enqueue_cmd(CMD_END_CONFIG)
    elif cmd == "fw":
        _config_wrap(sensor, (CMD_READ_FIRMWARE, b''))
    elif cmd == "sn" and len(parts) == 1:
        _config_wrap(sensor, (CMD_READ_SERIAL, b''))
    elif cmd == "sn" and len(parts) == 2:
        sn_bytes = parts[1].encode('ascii').ljust(8, b'\x00')[:8]
        _config_wrap(sensor, (CMD_WRITE_SERIAL, struct.pack('<H', 8) + sn_bytes))
    elif cmd == "common":
        param_words = b''.join(struct.pack('<H', pw) for pw, _ in COMMON_PARAM_NAMES)
        _config_wrap(sensor, (CMD_READ_COMMON, param_words))
    elif cmd == "threshold":
        gate_words = b''.join(struct.pack('<H', i) for i in range(0x10))
        _config_wrap(sensor, (CMD_READ_THRESHOLD, gate_words))
    elif cmd == "snr":
        gate_words = b''.join(struct.pack('<H', i) for i in range(0x10))
        _config_wrap(sensor, (CMD_READ_SNR, gate_words))
    elif cmd == "all":
        param_words = b''.join(struct.pack('<H', pw) for pw, _ in COMMON_PARAM_NAMES)
        gate_words = b''.join(struct.pack('<H', i) for i in range(0x10))
        _config_wrap(sensor,
            (CMD_READ_FIRMWARE, b''),
            (CMD_READ_SERIAL, b''),
            (CMD_READ_COMMON, param_words),
            (CMD_READ_THRESHOLD, gate_words),
            (CMD_READ_SNR, gate_words),
        )
    elif cmd == "set" and len(parts) == 3:
        name, value = parts[1], parts[2]
        if name not in ALL_PARAMS:
            print(f"Unknown param: {name}")
            return
        if name == "serial_number":
            sn_bytes = value.encode('ascii').ljust(8, b'\x00')[:8]
            _config_wrap(sensor, (CMD_WRITE_SERIAL, struct.pack('<H', 8) + sn_bytes))
        else:
            write_cmd, param_word = ALL_PARAMS[name]
            _config_wrap(sensor, (write_cmd, struct.pack('<H', param_word) + struct.pack('<I', int(value))))
    elif cmd == "calibrate":
        trigger = int(parts[1]) if len(parts) > 1 else 2
        retention = int(parts[2]) if len(parts) > 2 else 1
        duration = int(parts[3]) if len(parts) > 3 else 120
        _config_wrap(sensor, (CMD_CALIBRATE, struct.pack('<HHH', trigger, retention, duration)))
    elif cmd == "raw" and len(parts) >= 2:
        try:
            cmd_word = int(parts[1], 16)
            raw_data = bytes.fromhex(''.join(parts[2:])) if len(parts) > 2 else b''
            sensor.enqueue_cmd(cmd_word, raw_data)
        except ValueError:
            print(f"Bad hex: {' '.join(parts[1:])}")
    else:
        print(f"Unknown command: {line}")


def poll_cmd_file(sensor):
    """Check for command file, read it, delete it, execute the command."""
    try:
        with open(CMD_FILE, 'r') as f:
            line = f.read().strip()
        os.remove(CMD_FILE)
        if line:
            print(f"[FILE] {line}")
            process_command(sensor, line)
    except FileNotFoundError:
        pass


if __name__ == '__main__':
    sys.stdout.reconfigure(line_buffering=True)

    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    sensor = LD2410S(dev)
    sensor.start()

    try:
        while True:
            poll_cmd_file(sensor)
            time.sleep(0.2)
    except KeyboardInterrupt:
        sensor.enqueue_cmd(CMD_END_CONFIG)
        time.sleep(0.5)
