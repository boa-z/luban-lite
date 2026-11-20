"""
aicupg_client.py - Unified TCP Client SDK for upgcmd Server

Provides frame protocol implementation and high-level APIs aligned with Server.
Used by skill_burn and other modules.

Architecture:
    Agent -> aicupg_client -> TCP localhost:9100 -> upgcmd Server -> USB/UART -> DevBoard

Frame format:
    [Header 0xAA55][Version 1B][CmdType 1B][Length 4B LE][JSON Payload][CRC16 2B LE][Tail 0x55AA]

Command types (aligned with tcpserver/src/cmd_handler.h):
    CMD_CONFIG_DEVICE    = 0x01
    CMD_GET_DEVICE_LIST  = 0x02
    CMD_GET_HW_INFO      = 0x03
    CMD_BURN_IMAGE       = 0x10
    CMD_CANCEL_BURN      = 0x12
    CMD_GET_LOG          = 0x20
    CMD_SEND_CMD         = 0x21
    CMD_LOG_STREAM       = 0x22  # Server push
    CMD_START_LOG_MONITOR = 0x23
    CMD_STOP_LOG_MONITOR  = 0x24
    CMD_SERIAL_WRITE      = 0x25  # Write to serial (during monitor)
    CMD_HELP             = 0x30
    CMD_PROTO_VERSION    = 0x31
    CMD_HEARTBEAT        = 0x32
    CMD_UPLOAD_FIRMWARE  = 0x40  # Upload firmware metadata (JSON)
    CMD_FIRMWARE_CHUNK   = 0x41  # Transfer firmware data chunk (binary payload)
    CMD_RSP_MASK         = 0x80  # Response flag
"""

import socket
import struct
import json
import time
import threading
import queue
import traceback
from typing import Optional, Dict, Any, Tuple, List


# ==================== Frame Protocol Constants ====================
FRAME_MAGIC_HIGH = 0xAA
FRAME_MAGIC_LOW = 0x55
FRAME_VERSION = 0x01
FRAME_TAIL_HIGH = 0x55
FRAME_TAIL_LOW = 0xAA

FRAME_HEADER_SIZE = 8
FRAME_FOOTER_SIZE = 4
FRAME_MIN_SIZE = FRAME_HEADER_SIZE + FRAME_FOOTER_SIZE

CMD_CONFIG_DEVICE = 0x01
CMD_GET_DEVICE_LIST = 0x02
CMD_GET_HW_INFO = 0x03
CMD_BURN_IMAGE = 0x10
CMD_CANCEL_BURN = 0x12
CMD_GET_LOG = 0x20
CMD_SEND_CMD = 0x21
CMD_HELP = 0x30
CMD_PROTO_VERSION = 0x31
CMD_HEARTBEAT = 0x32
CMD_LOG_STREAM = 0x22
CMD_START_LOG_MONITOR = 0x23
CMD_STOP_LOG_MONITOR = 0x24
CMD_SERIAL_WRITE = 0x25
CMD_UPLOAD_FIRMWARE = 0x40
CMD_FIRMWARE_CHUNK = 0x41
CMD_UPLOAD_FIRMWARE_FINISH = 0x42
CMD_GET_UPLOAD_DIR = 0x43

CMD_RSP_MASK = 0x80


def crc16_ccitt(data: bytes) -> int:
    """
    Calculate CRC16-CCITT checksum.

    Args:
        data: Data to checksum

    Returns:
        CRC16 value (16-bit)
    """
    crc = 0xFFFF
    poly = 0x1021

    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ poly) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF

    return crc


def encode_frame(cmd_type: int, payload: bytes) -> bytes:
    """
    Encode TCP frame.

    Frame: AA55 + version(1B) + cmd_type(1B) + length(4B LE) + payload + CRC16(2B LE) + 55AA

    Args:
        cmd_type: Command type
        payload: JSON Payload (bytes)

    Returns:
        Complete binary frame
    """
    header = bytes([FRAME_MAGIC_HIGH, FRAME_MAGIC_LOW])
    header += bytes([FRAME_VERSION])
    header += bytes([cmd_type])
    header += struct.pack('<I', len(payload))

    crc_data = header + payload
    crc = crc16_ccitt(crc_data)

    footer = struct.pack('<H', crc)
    footer += bytes([FRAME_TAIL_HIGH, FRAME_TAIL_LOW])

    return header + payload + footer


def decode_frame(data: bytes) -> Optional[Tuple[int, bytes]]:
    """
    Decode TCP frame.

    Args:
        data: Complete binary frame

    Returns:
        (cmd_type, payload) or None if frame is incomplete/CRC failed
    """
    if len(data) < FRAME_MIN_SIZE:
        return None

    if data[0] != FRAME_MAGIC_HIGH or data[1] != FRAME_MAGIC_LOW:
        print(f"[AICUPGClient] Invalid magic: {hex(data[0])}{hex(data[1])}")
        return None

    if data[-2] != FRAME_TAIL_HIGH or data[-1] != FRAME_TAIL_LOW:
        print(f"[AICUPGClient] Invalid tail: {hex(data[-2])}{hex(data[-1])}")
        return None

    version = data[2]
    cmd_type = data[3]
    payload_len = struct.unpack('<I', data[4:8])[0]

    if version != FRAME_VERSION:
        print(f"[AICUPGClient] Unsupported version: {version}")
        return None

    payload_start = FRAME_HEADER_SIZE
    payload_end = payload_start + payload_len

    if len(data) < payload_end + FRAME_FOOTER_SIZE:
        print(
            f"[AICUPGClient] Frame too short: expected {payload_end + FRAME_FOOTER_SIZE}, "
            f"got {len(data)}"
        )
        return None

    payload = data[payload_start:payload_end]

    crc_data = data[0:payload_end]
    received_crc = struct.unpack('<H', data[payload_end:payload_end+2])[0]
    calculated_crc = crc16_ccitt(crc_data)

    if received_crc != calculated_crc:
        print(
            f"[AICUPGClient] CRC mismatch: received={hex(received_crc)}, "
            f"calculated={hex(calculated_crc)}"
        )
        return None

    return (cmd_type, payload)


class AICUPGClient:
    """
    upgcmd Server TCP client.

    Provides high-level API, hiding frame protocol details.
    """

    def __init__(self, host: str = 'localhost', port: int = 9100, timeout: float = 5.0):
        """
        Initialize client.

        Args:
            host: Server address (default localhost)
            port: Server port (default 9100)
            timeout: Socket timeout in seconds
        """
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self.connected = False
        self._task_id_counter = 0

        self._recv_thread: Optional[threading.Thread] = None
        self._recv_stop_event = threading.Event()
        self._response_queue: queue.Queue = queue.Queue()
        self._stream_queue: queue.Queue = queue.Queue()
        self._log_callback = None

    def connect(self) -> bool:
        """
        Connect to upgcmd Server.

        Returns:
            True on success, False on failure
        """
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect((self.host, self.port))
            self.connected = True
            print(f"[AICUPGClient] Connected to {self.host}:{self.port}")
            self._start_recv_thread()
            return True
        except Exception as e:
            print(f"[AICUPGClient] Connection failed: {e}")
            self.connected = False
            return False

    def disconnect(self):
        """Disconnect from server."""
        self._stop_recv_thread()
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        self.connected = False
        print("[AICUPGClient] Disconnected")

    def _send_frame(self, cmd_type: int, payload: bytes) -> bool:
        """
        Send frame.

        Args:
            cmd_type: Command type
            payload: JSON Payload (bytes)

        Returns:
            True on success, False on failure
        """
        if not self.connected or not self.sock:
            print("[AICUPGClient] Not connected")
            return False

        frame = encode_frame(cmd_type, payload)

        try:
            self.sock.sendall(frame)
            return True
        except Exception as e:
            print(f"[AICUPGClient] Send failed: {e}")
            return False

    def _recv_frame(self, timeout: Optional[float] = None) -> Optional[Tuple[int, bytes]]:
        """
        Receive response frame (from background recv thread's response queue).

        Args:
            timeout: Timeout in seconds, None uses default

        Returns:
            (cmd_type, payload) or None on timeout/error
        """
        if not self.connected:
            return None

        tmo = timeout if timeout is not None else self.timeout
        try:
            result = self._response_queue.get(timeout=tmo)
            return result
        except queue.Empty:
            print("[AICUPGClient] Receive timeout (response queue empty)")
            return None

    # ==================== Background Recv Thread ====================

    def _start_recv_thread(self):
        """Start background recv thread."""
        if self._recv_thread and self._recv_thread.is_alive():
            return
        self._recv_stop_event.clear()
        while not self._response_queue.empty():
            try:
                self._response_queue.get_nowait()
            except queue.Empty:
                break
        while not self._stream_queue.empty():
            try:
                self._stream_queue.get_nowait()
            except queue.Empty:
                break
        self._recv_thread = threading.Thread(
            target=self._recv_thread_loop, daemon=True, name="AICUPGRecv"
        )
        self._recv_thread.start()
        print("[AICUPGClient] Recv thread started")

    def _stop_recv_thread(self):
        """Stop background recv thread."""
        if self._recv_thread and self._recv_thread.is_alive():
            self._recv_stop_event.set()
            self._recv_thread.join(timeout=3.0)
            print("[AICUPGClient] Recv thread stopped")
        self._recv_thread = None

    def _recv_thread_loop(self):
        """
        Background recv thread main loop.
        Continuously receives data from socket, parses frames,
        dispatches to response queue or stream queue by type.
        """
        buf = b''
        while not self._recv_stop_event.is_set():
            if not self.sock:
                break
            try:
                self.sock.settimeout(0.5)
                chunk = self.sock.recv(4096)
                if not chunk:
                    print("[AICUPGClient] Connection closed by server")
                    self.connected = False
                    break
                buf += chunk

                while len(buf) >= FRAME_MIN_SIZE:
                    if buf[0] != FRAME_MAGIC_HIGH or buf[1] != FRAME_MAGIC_LOW:
                        idx = buf.find(bytes([FRAME_MAGIC_HIGH, FRAME_MAGIC_LOW]), 1)
                        if idx == -1:
                            buf = b''
                            break
                        buf = buf[idx:]
                        continue

                    if len(buf) < FRAME_HEADER_SIZE:
                        break

                    payload_len = struct.unpack('<I', buf[4:8])[0]
                    frame_total = FRAME_HEADER_SIZE + payload_len + FRAME_FOOTER_SIZE

                    if len(buf) < frame_total:
                        break

                    frame_data = buf[:frame_total]
                    buf = buf[frame_total:]

                    result = decode_frame(frame_data)
                    if result is None:
                        continue

                    cmd_type, payload = result

                    if cmd_type & CMD_RSP_MASK:
                        self._response_queue.put((cmd_type, payload))
                    elif cmd_type == CMD_LOG_STREAM:
                        try:
                            log_obj = json.loads(payload.decode('utf-8'))
                            log_text = log_obj.get('data', '')
                        except Exception:
                            log_text = payload.decode('utf-8', errors='replace')
                        self._stream_queue.put((cmd_type, log_text))
                        if self._log_callback:
                            try:
                                self._log_callback(log_text)
                            except Exception as e:
                                print(f"[AICUPGClient] Log callback error: {e}")
                    else:
                        self._stream_queue.put((cmd_type, payload))

            except socket.timeout:
                continue
            except Exception as e:
                if not self._recv_stop_event.is_set():
                    print(f"[AICUPGClient] Recv thread error: {e}")
                    traceback.print_exc()
                break

        print("[AICUPGClient] Recv thread exiting")

    def set_log_callback(self, callback):
        """
        Set real-time log callback.

        Args:
            callback: callback(log_text: str), None to cancel
        """
        self._log_callback = callback

    def _next_task_id(self) -> int:
        """Generate next task ID."""
        self._task_id_counter += 1
        return self._task_id_counter

    # ==================== High-Level API ====================

    def heartbeat(self) -> Optional[Dict[str, Any]]:
        """
        Heartbeat check.

        Returns:
            Response dict or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_HEARTBEAT, payload):
            return None

        response = self._recv_frame()
        if response is None:
            return None

        cmd_type, resp_payload = response

        expected_cmd = CMD_HEARTBEAT | CMD_RSP_MASK
        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def get_proto_version(self) -> Optional[Dict[str, Any]]:
        """
        Get protocol version.

        Returns:
            Response dict or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_PROTO_VERSION, payload):
            return None

        response = self._recv_frame()
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_PROTO_VERSION | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def config_device(self, port: str, baudrate: int = 0) -> Optional[Dict[str, Any]]:
        """
        Configure device (USB or UART).

        Args:
            port: Device port
                  - USB: "bus:port" format (e.g. "1:13")
                  - UART: COM port (e.g. "COM4")
            baudrate: Baud rate (only for UART, set 0 for USB)

        Returns:
            Response dict: {"status": "success", "data": {...}} or None
        """
        params = {
            "task_id": self._next_task_id(),
            "params": {
                "port": port,
                "baudrate": baudrate
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_CONFIG_DEVICE, payload):
            return None

        response = self._recv_frame(timeout=10.0)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_CONFIG_DEVICE | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def get_hw_info(self) -> Optional[Dict[str, Any]]:
        """
        Get hardware info (requires config_device first).

        Returns:
            Response dict: {"status": "success", "data": {...}} or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_GET_HW_INFO, payload):
            return None

        response = self._recv_frame()
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_GET_HW_INFO | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def upload_firmware(self, local_path: str, remote_dir: str = "C:\\temp\\aicupg_firmware\\",
                        chunk_size: int = 8192, progress_callback=None) -> Optional[Dict[str, Any]]:
        """
        Upload firmware file to Server's local temp directory (for remote Agent firmware transfer).

        Args:
            local_path: Agent local firmware path (may be Linux path e.g. /home/...)
            remote_dir: Server save directory (Windows path, e.g. C:\\temp\\aicupg_firmware\\)
            chunk_size: Chunk size in bytes, default 8KB
            progress_callback: Optional callback(progress: float, received: int, total: int)

        Returns:
            Response dict:
            {
                "status": "success",
                "remote_path": "C:\\temp\\aicupg_firmware\\<timestamp>\\firmware.img",
                "file_size": 10485760
            }
        """
        import os
        from datetime import datetime

        if not os.path.exists(local_path):
            print(f"[AICUPGClient] Firmware file not found: {local_path}")
            return None

        file_size = os.path.getsize(local_path)
        filename = os.path.basename(local_path)

        if remote_dir.endswith('\\') or remote_dir.endswith('/'):
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            remote_dir = f"{remote_dir}{timestamp}"

        print(f"[AICUPGClient] Starting firmware upload: {filename} ({file_size} bytes)")
        print(f"[AICUPGClient] Remote directory: {remote_dir}")

        params = {
            "filename": filename,
            "file_size": file_size,
            "remote_dir": remote_dir
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_UPLOAD_FIRMWARE, payload):
            return None

        response = self._recv_frame(timeout=10.0)
        if response is None:
            return None

        cmd_type, resp_payload = response

        if cmd_type != (CMD_UPLOAD_FIRMWARE | CMD_RSP_MASK):
            print(f"[AICUPGClient] Unexpected response: 0x{cmd_type:02X}")
            return None

        try:
            resp_data = json.loads(resp_payload.decode('utf-8'))
        except Exception as e:
            print(f"[AICUPGClient] Failed to parse response JSON: {e}")
            return None

        if resp_data.get('status') != 'success':
            print(f"[AICUPGClient] Upload start failed: {resp_data}")
            return None

        remote_path = resp_data.get('data', {}).get('remote_path', '')
        print(f"[AICUPGClient] Server ready, remote path: {remote_path}")

        received_size = 0

        with open(local_path, 'rb') as f:
            while received_size < file_size:
                chunk_data = f.read(chunk_size)
                if not chunk_data:
                    break

                if not self._send_frame(CMD_FIRMWARE_CHUNK, chunk_data):
                    print(f"[AICUPGClient] Failed to send chunk at offset {received_size}")
                    return None

                response = self._recv_frame(timeout=5.0)
                if response is None:
                    print(f"[AICUPGClient] No response for chunk at offset {received_size}")
                    return None

                cmd_type, resp_payload = response

                if cmd_type != (CMD_FIRMWARE_CHUNK | CMD_RSP_MASK):
                    print(f"[AICUPGClient] Unexpected chunk response: 0x{cmd_type:02X}")
                    return None

                try:
                    resp_data = json.loads(resp_payload.decode('utf-8'))
                except Exception as e:
                    print(f"[AICUPGClient] Failed to parse chunk response JSON: {e}")
                    return None

                if resp_data.get('status') != 'success':
                    print(f"[AICUPGClient] Chunk upload failed: {resp_data}")
                    return None

                received_size = int(resp_data.get('data', {}).get('received_size', received_size))

                if progress_callback:
                    progress = received_size * 100 / file_size
                    progress_callback(progress, received_size, file_size)

                if received_size % (chunk_size * 10) == 0:
                    progress_pct = received_size * 100 / file_size
                    print(
                        f"[AICUPGClient] Uploaded {received_size}/{file_size} bytes "
                        f"({progress_pct:.1f}%)"
                    )

        print(f"[AICUPGClient] Firmware upload completed: {received_size} bytes")

        print(f"[AICUPGClient] Sending finish command...")
        params = {
            "task_id": self._next_task_id()
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_UPLOAD_FIRMWARE_FINISH, payload):
            print(f"[AICUPGClient] Failed to send finish command")
            return None

        response = self._recv_frame(timeout=5.0)
        if response is None:
            print(f"[AICUPGClient] No response for finish command")
            return None

        cmd_type, resp_payload = response

        if cmd_type != (CMD_UPLOAD_FIRMWARE_FINISH | CMD_RSP_MASK):
            print(f"[AICUPGClient] Unexpected finish response: 0x{cmd_type:02X}")
            return None

        try:
            resp_data = json.loads(resp_payload.decode('utf-8'))
        except Exception as e:
            print(f"[AICUPGClient] Failed to parse finish response JSON: {e}")
            return None

        if resp_data.get('status') != 'success':
            print(f"[AICUPGClient] Finish failed: {resp_data}")
            return None

        print(f"[AICUPGClient] ✅ Upload finished successfully")

        return {
            'status': 'success',
            'remote_path': remote_path,
            'file_size': file_size
        }

    def get_upload_dir(self) -> Optional[str]:
        """
        Get Server-recommended firmware upload directory.

        Server auto-selects a path with write permission and available space
        (e.g. %TEMP%, %APPDATA%, etc.)

        Returns:
            Server path string, or None on failure
        """
        params = {
            "task_id": self._next_task_id()
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_GET_UPLOAD_DIR, payload):
            print(f"[AICUPGClient] Failed to send GET_UPLOAD_DIR command")
            return None

        response = self._recv_frame(timeout=5.0)
        if response is None:
            print(f"[AICUPGClient] No response for GET_UPLOAD_DIR")
            return None

        cmd_type, resp_payload = response

        if cmd_type != (CMD_GET_UPLOAD_DIR | CMD_RSP_MASK):
            print(f"[AICUPGClient] Unexpected response: 0x{cmd_type:02X}")
            return None

        try:
            resp_data = json.loads(resp_payload.decode('utf-8'))
        except Exception as e:
            print(f"[AICUPGClient] Failed to parse response JSON: {e}")
            return None

        if resp_data.get('status') != 'success':
            print(f"[AICUPGClient] Failed to get upload dir: {resp_data}")
            return None

        upload_dir = resp_data.get('data', {}).get('upload_dir', '')
        auto_selected = resp_data.get('data', {}).get('auto_selected', False)

        if auto_selected:
            print(f"[AICUPGClient] Server auto-selected upload dir: {upload_dir}")
        else:
            print(f"[AICUPGClient] Server upload dir: {upload_dir}")

        return upload_dir

    def get_device_list(self) -> Optional[List[Dict[str, Any]]]:
        """
        Get USB device list.

        Returns:
            Device list, e.g. [{"port": "1:13", "chip_type": "D21x", ...}]
            Returns None on failure
        """
        params = {
            "task_id": self._next_task_id()
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_GET_DEVICE_LIST, payload):
            print(f"[AICUPGClient] Failed to send GET_DEVICE_LIST command")
            return None

        response = self._recv_frame(timeout=5.0)
        if response is None:
            print(f"[AICUPGClient] No response for GET_DEVICE_LIST")
            return None

        cmd_type, resp_payload = response

        if cmd_type != (CMD_GET_DEVICE_LIST | CMD_RSP_MASK):
            print(f"[AICUPGClient] Unexpected response: 0x{cmd_type:02X}")
            return None

        try:
            resp_data = json.loads(resp_payload.decode('utf-8'))
        except Exception as e:
            print(f"[AICUPGClient] Failed to parse response JSON: {e}")
            return None

        if resp_data.get('status') != 'success':
            print(f"[AICUPGClient] Failed to get device list: {resp_data}")
            return None

        devices = resp_data.get('data', [])
        print(f"[AICUPGClient] Found {len(devices)} USB device(s)")
        for i, dev in enumerate(devices):
            port = dev.get('port')
            chip = dev.get('chip_type')
            print(f"[AICUPGClient]   Device {i+1}: port={port}, chip={chip}")

        return devices

    def burn_image(self, image_path: str, auto_reboot: bool = True,
                   partition_mode: str = "all") -> Optional[Dict[str, Any]]:
        """
        Burn firmware image (requires config_device first).

        Args:
            image_path: Firmware file path (Windows or Unix path)
            auto_reboot: Auto reboot after burn
            partition_mode: Partition mode ("all" or other)

        Returns:
            Response dict: {"status": "success", ...} or None
        """
        normalized_path = image_path.replace('\\', '/')

        params = {
            "task_id": self._next_task_id(),
            "params": {
                "image_path": normalized_path,
                "auto_reboot": auto_reboot,
                "partition_mode": partition_mode
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_BURN_IMAGE, payload):
            return None

        response = self._recv_frame(timeout=120.0)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_BURN_IMAGE | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def send_cmd(self, command: str) -> Optional[Dict[str, Any]]:
        """
        Send command to device (requires config_device first).

        Args:
            command: Command string (e.g. "read_reg 0x1000" or "make all")

        Returns:
            Response dict: {"status": "success", "data": {...}} or None
        """
        params = {
            "task_id": self._next_task_id(),
            "params": {
                "command": command
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_SEND_CMD, payload):
            return None

        is_reset = command.strip().lower() in ('reset', 'reboot')
        recv_timeout = 10.0 if is_reset else 60.0
        response = self._recv_frame(timeout=recv_timeout)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_SEND_CMD | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def get_log(self, timeout: int = 5) -> Optional[Dict[str, Any]]:
        """
        Get boot log (requires config_device first).

        Args:
            timeout: Max read time in seconds

        Returns:
            Response dict: {"status": "success", "data": {"log": "..."}} or None
        """
        params = {
            "task_id": self._next_task_id(),
            "params": {
                "timeout_sec": timeout
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_GET_LOG, payload):
            return None

        response = self._recv_frame(timeout=float(timeout + 5))
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_GET_LOG | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def cancel_burn(self) -> Optional[Dict[str, Any]]:
        """
        Cancel current burn task.

        Returns:
            Response dict or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_CANCEL_BURN, payload):
            return None

        response = self._recv_frame()
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_CANCEL_BURN | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def help(self) -> Optional[Dict[str, Any]]:
        """
        Get help information.

        Returns:
            Response dict or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_HELP, payload):
            return None

        response = self._recv_frame()
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_HELP | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    # ==================== Serial Log Monitor API ====================

    def start_log_monitor(self, port: str, baudrate: int = 115200,
                          timeout_sec: int = 0) -> Optional[Dict[str, Any]]:
        """
        Start serial log monitor.

        Server opens the specified serial port independently, creates a
        monitor thread, and continuously pushes raw serial data via
        CMD_LOG_STREAM frames to the client.

        Args:
            port: Serial port (e.g. "COM5", "/dev/ttyUSB1")
            baudrate: Baud rate (default 115200)
            timeout_sec: Max monitor time in seconds, 0=unlimited

        Returns:
            Response dict: {"status": "success", ...} or None
        """
        params = {
            "task_id": self._next_task_id(),
            "params": {
                "port": port,
                "baudrate": baudrate,
                "timeout_sec": timeout_sec
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_START_LOG_MONITOR, payload):
            return None

        response = self._recv_frame(timeout=10.0)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_START_LOG_MONITOR | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def stop_log_monitor(self) -> Optional[Dict[str, Any]]:
        """
        Stop serial log monitor.

        Returns:
            Response dict or None
        """
        payload = json.dumps({"task_id": self._next_task_id()}).encode('utf-8')

        if not self._send_frame(CMD_STOP_LOG_MONITOR, payload):
            return None

        response = self._recv_frame(timeout=5.0)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_STOP_LOG_MONITOR | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def send_serial_command(
        self, command: str, auto_newline: bool = True
    ) -> Optional[Dict[str, Any]]:
        """
        Send command to device via serial port (requires monitor running).

        Command is written to serial port. Device response is captured by
        the monitor thread and pushed back via CMD_LOG_STREAM,
        retrievable with collect_logs().

        Args:
            command: Command string (e.g. "free", "ls /tmp")
            auto_newline: Auto-append \\n (default True, needed for shell commands)

        Returns:
            Response dict: {"status": "success", "message": "Serial write OK (5 bytes)"} or None
        """
        if auto_newline and not command.endswith('\n'):
            command = command + '\n'

        params = {
            "task_id": self._next_task_id(),
            "params": {
                "command": command
            }
        }
        payload = json.dumps(params).encode('utf-8')

        if not self._send_frame(CMD_SERIAL_WRITE, payload):
            return None

        response = self._recv_frame(timeout=5.0)
        if response is None:
            return None

        cmd_type, resp_payload = response
        expected_cmd = CMD_SERIAL_WRITE | CMD_RSP_MASK

        if cmd_type != expected_cmd:
            print(f"[AICUPGClient] Unexpected response type: {hex(cmd_type)}")
            return None

        try:
            return json.loads(resp_payload.decode('utf-8'))
        except Exception:
            return None

    def collect_logs(self, duration_sec: float = 5.0,
                     stop_on_pattern: Optional[str] = None,
                     callback=None) -> List[str]:
        """
        Collect serial log data.

        Reads from _stream_queue with two stop conditions:
        1. Time elapsed (duration_sec)
        2. Pattern matched (stop_on_pattern)
        3. duration_sec=0 returns all accumulated logs immediately (no wait)

        Args:
            duration_sec: Max wait time in seconds, 0=return all accumulated logs immediately
            stop_on_pattern: Stop early when this string is found (e.g. shell prompt "# ")
            callback: Optional real-time callback callback(line: str), called per line

        Returns:
            List of log lines
        """
        logs = []

        if duration_sec == 0:
            return self.drain_stream_queue()

        deadline = time.time() + duration_sec

        while time.time() < deadline:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            try:
                tmo = min(0.5, remaining)
                cmd_type, log_text = self._stream_queue.get(timeout=tmo)
                if log_text:
                    lines = log_text.splitlines()
                    for line in lines:
                        logs.append(line)
                        if callback:
                            try:
                                callback(line)
                            except Exception:
                                pass
                        if stop_on_pattern and stop_on_pattern in line:
                            return logs
            except queue.Empty:
                continue

        return logs

    def drain_stream_queue(self) -> List[str]:
        """
        Drain stream queue, return all accumulated logs.

        Returns:
            List of log lines
        """
        logs = []
        while not self._stream_queue.empty():
            try:
                cmd_type, log_text = self._stream_queue.get_nowait()
                if isinstance(log_text, str) and log_text:
                    logs.extend(log_text.splitlines())
            except queue.Empty:
                break
        return logs


# ==================== Utility Functions ====================

def test_connection(host: str = 'localhost', port: int = 9100) -> bool:
    """
    Test if connection is available.

    Args:
        host: Server address
        port: Server port

    Returns:
        True if connection and heartbeat succeed, False otherwise
    """
    client = AICUPGClient(host, port)

    if not client.connect():
        return False

    try:
        result = client.heartbeat()
        if result and result.get('status') == 'success':
            print("[test_connection] Heartbeat OK")
            return True
        else:
            print("[test_connection] Heartbeat failed")
            return False
    finally:
        client.disconnect()


if __name__ == '__main__':
    print("Testing AICUPGClient...")

    if test_connection():
        print("\n✓ Connection test passed!")
    else:
        print("\n✗ Connection test failed!")
        print("  Please ensure upgcmd Server is running on localhost:9100")
