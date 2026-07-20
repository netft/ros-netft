import queue
import socket
import struct
import threading
import time

from netft_driver.protocol import Command, RECORD_STRUCT, REQUEST_STRUCT


class FakeNetFTSensor:
    def __init__(self, rate_hz=200.0):
        self.rate_hz = float(rate_hz)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(("127.0.0.1", 0))
        self._socket.settimeout(min(0.002, 0.25 / self.rate_hz))
        self._stop = threading.Event()
        self._enabled = threading.Event()
        self._enabled.set()
        self._thread = None
        self._streaming = False
        self._client_address = None
        self._requests = []
        self._commands_lock = threading.Lock()
        self._payloads = queue.Queue()
        self._rdt_sequence = 0
        self._ft_sequence = 1000
        self._rdt_skip = 0

    @property
    def host(self):
        return self._socket.getsockname()[0]

    @property
    def port(self):
        return self._socket.getsockname()[1]

    @property
    def commands(self):
        with self._commands_lock:
            return tuple(command for command, sample_count in self._requests)

    @property
    def requests(self):
        with self._commands_lock:
            return tuple(self._requests)

    def start(self):
        self._thread = threading.Thread(
            target=self._run, name="fake-netft", daemon=True
        )
        self._thread.start()
        return self

    def close(self):
        self._stop.set()
        self._socket.close()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def __enter__(self):
        return self.start()

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def pause(self):
        self._enabled.clear()

    def resume(self):
        self._enabled.set()

    def queue_payload(self, payload):
        self._payloads.put(bytes(payload))

    def send_payload_now(self, payload):
        if self._client_address is None:
            raise RuntimeError("fake sensor has no streaming client")
        self._socket.sendto(bytes(payload), self._client_address)

    def queue_record(
        self,
        rdt_sequence,
        status=0,
        axes=(100, -200, 300, 10, -20, 30),
        ft_sequence=None,
    ):
        if ft_sequence is None:
            ft_sequence = self._ft_sequence
        payload = RECORD_STRUCT.pack(
            rdt_sequence & 0xFFFFFFFF,
            ft_sequence & 0xFFFFFFFF,
            status & 0xFFFFFFFF,
            *axes
        )
        self._ft_sequence = (ft_sequence + 4) & 0xFFFFFFFF
        self.queue_payload(payload)

    def skip_rdt(self, count):
        self._rdt_skip += int(count)

    def wait_for_command(self, command, count=1, timeout=1.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.commands.count(command) >= count:
                return True
            time.sleep(0.005)
        return False

    def _record_request(self, command, sample_count):
        with self._commands_lock:
            self._requests.append((command, sample_count))

    def _handle_request(self, payload, address):
        if len(payload) != REQUEST_STRUCT.size:
            return
        header, command_value, sample_count = REQUEST_STRUCT.unpack(payload)
        if header != 0x1234:
            return
        try:
            command = Command(command_value)
        except ValueError:
            return
        self._record_request(command, sample_count)
        if command in (Command.START_REALTIME, Command.START_BUFFERED):
            self._client_address = address
            self._streaming = True
            self._rdt_sequence = 0
        elif command in (Command.STOP_STREAMING, Command.SET_SOFTWARE_BIAS):
            self._streaming = False

    def _next_payload(self):
        try:
            payload = self._payloads.get_nowait()
            if len(payload) == RECORD_STRUCT.size:
                self._rdt_sequence = struct.unpack(">I", payload[:4])[0]
            return payload
        except queue.Empty:
            self._rdt_sequence = (
                self._rdt_sequence + 1 + self._rdt_skip
            ) & 0xFFFFFFFF
            self._rdt_skip = 0
            payload = RECORD_STRUCT.pack(
                self._rdt_sequence,
                self._ft_sequence,
                0,
                100,
                -200,
                300,
                10,
                -20,
                30,
            )
            self._ft_sequence = (self._ft_sequence + 4) & 0xFFFFFFFF
            return payload

    def _run(self):
        interval = 1.0 / self.rate_hz
        next_send = time.monotonic()
        while not self._stop.is_set():
            try:
                payload, address = self._socket.recvfrom(64)
                self._handle_request(payload, address)
            except socket.timeout:
                pass
            except OSError:
                break
            now = time.monotonic()
            if (
                self._streaming
                and self._enabled.is_set()
                and self._client_address is not None
                and now >= next_send
            ):
                try:
                    self._socket.sendto(self._next_payload(), self._client_address)
                except OSError:
                    break
                next_send = max(next_send + interval, now)
