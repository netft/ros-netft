import math
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass
from enum import Enum
from typing import Callable, Deque, Optional, Tuple

from .protocol import (
    Command,
    ProtocolError,
    RawRecord,
    decode_record,
    encode_request,
)
from .status import (
    DiagnosticSeverity,
    FtSequenceKind,
    FtSequenceTracker,
    RdtSequenceTracker,
    SequenceKind,
    classify_status,
)


_RECEIVE_POLL_INTERVAL = 0.05


class ClientState(Enum):
    STOPPED = "stopped"
    CONNECTING = "connecting"
    STREAMING = "streaming"
    BACKOFF = "backoff"


class NotConnectedError(ConnectionError):
    pass


def _positive_finite(name, value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise TypeError("{} must be numeric".format(name))
    value = float(value)
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError("{} must be finite and greater than zero".format(name))
    return value


@dataclass(frozen=True)
class ClientConfig:
    sensor_host: str
    sensor_port: int = 49152
    counts_per_force: float = 1_000_000.0
    counts_per_torque: float = 1_000_000.0
    publish_rate: float = 0.0
    receive_timeout: float = 0.1
    reconnect_initial_delay: float = 0.25
    reconnect_max_delay: float = 5.0
    publish_on_error: bool = False

    def __post_init__(self):
        if not isinstance(self.sensor_host, str) or not self.sensor_host.strip():
            raise ValueError("sensor_host must be non-empty")
        if (
            not isinstance(self.sensor_port, int)
            or isinstance(self.sensor_port, bool)
            or not 1 <= self.sensor_port <= 65535
        ):
            raise ValueError("sensor_port must be between 1 and 65535")
        _positive_finite("counts_per_force", self.counts_per_force)
        _positive_finite("counts_per_torque", self.counts_per_torque)
        if (
            not isinstance(self.publish_rate, (int, float))
            or isinstance(self.publish_rate, bool)
            or not math.isfinite(float(self.publish_rate))
            or float(self.publish_rate) < 0.0
        ):
            raise ValueError("publish_rate must be finite and non-negative")
        initial = _positive_finite(
            "reconnect_initial_delay", self.reconnect_initial_delay
        )
        maximum = _positive_finite("reconnect_max_delay", self.reconnect_max_delay)
        _positive_finite("receive_timeout", self.receive_timeout)
        if maximum < initial:
            raise ValueError(
                "reconnect_max_delay must be at least reconnect_initial_delay"
            )
        if not isinstance(self.publish_on_error, bool):
            raise TypeError("publish_on_error must be bool")


@dataclass(frozen=True)
class WrenchSample:
    rdt_sequence: int
    ft_sequence: int
    status: int
    force: Tuple[float, float, float]
    torque: Tuple[float, float, float]
    received_monotonic: float


@dataclass(frozen=True)
class HealthSnapshot:
    state: ClientState
    sensor_host: str
    sensor_port: int
    last_rdt_sequence: Optional[int]
    last_ft_sequence: Optional[int]
    last_status: int
    receive_rate: float
    publish_rate: float
    received_count: int
    published_count: int
    rate_dropped_count: int
    device_error_count: int
    lost_count: int
    duplicate_count: int
    out_of_order_count: int
    malformed_count: int
    reconnect_count: int
    timeout_count: int
    callback_error_count: int
    last_record_age: Optional[float]
    last_error: str
    ft_stall_count: int = 0
    ft_backward_count: int = 0
    ft_restart_count: int = 0
    last_ft_progress: str = "unknown"


class NetFTClient:
    def __init__(self, config):
        if not isinstance(config, ClientConfig):
            raise TypeError("config must be ClientConfig")
        self.config = config
        self._lock = threading.RLock()
        self._state = ClientState.STOPPED
        self._last_rdt_sequence = None
        self._last_ft_sequence = None
        self._last_status = 0
        self._received_count = 0
        self._published_count = 0
        self._rate_dropped_count = 0
        self._device_error_count = 0
        self._lost_count = 0
        self._duplicate_count = 0
        self._out_of_order_count = 0
        self._malformed_count = 0
        self._reconnect_count = 0
        self._timeout_count = 0
        self._callback_error_count = 0
        self._ft_stall_count = 0
        self._ft_backward_count = 0
        self._ft_restart_count = 0
        self._last_ft_progress = "unknown"
        self._last_valid_monotonic = None
        self._last_error = ""
        self._receive_times = deque()  # type: Deque[float]
        self._publish_times = deque()  # type: Deque[float]
        self._sequence_tracker = RdtSequenceTracker()
        self._ft_sequence_tracker = FtSequenceTracker()
        self._socket = None  # type: Optional[socket.socket]
        self._thread = None  # type: Optional[threading.Thread]
        self._stop_event = threading.Event()
        self._callback = None  # type: Optional[Callable[[WrenchSample], None]]
        self._last_publish_monotonic = None  # type: Optional[float]
        self._reconnect_delay = float(self.config.reconnect_initial_delay)

    def _convert_record(self, record, received_monotonic):
        force_scale = float(self.config.counts_per_force)
        torque_scale = float(self.config.counts_per_torque)
        return WrenchSample(
            rdt_sequence=record.rdt_sequence,
            ft_sequence=record.ft_sequence,
            status=record.status,
            force=(
                record.fx / force_scale,
                record.fy / force_scale,
                record.fz / force_scale,
            ),
            torque=(
                record.tx / torque_scale,
                record.ty / torque_scale,
                record.tz / torque_scale,
            ),
            received_monotonic=received_monotonic,
        )

    @staticmethod
    def _prune_rate_window(values, now):
        cutoff = now - 1.0
        while values and values[0] < cutoff:
            values.popleft()

    def health_snapshot(self):
        now = time.monotonic()
        with self._lock:
            self._prune_rate_window(self._receive_times, now)
            self._prune_rate_window(self._publish_times, now)
            age = (
                None
                if self._last_valid_monotonic is None
                else max(0.0, now - self._last_valid_monotonic)
            )
            return HealthSnapshot(
                state=self._state,
                sensor_host=self.config.sensor_host,
                sensor_port=self.config.sensor_port,
                last_rdt_sequence=self._last_rdt_sequence,
                last_ft_sequence=self._last_ft_sequence,
                last_status=self._last_status,
                receive_rate=float(len(self._receive_times)),
                publish_rate=float(len(self._publish_times)),
                received_count=self._received_count,
                published_count=self._published_count,
                rate_dropped_count=self._rate_dropped_count,
                device_error_count=self._device_error_count,
                lost_count=self._lost_count,
                duplicate_count=self._duplicate_count,
                out_of_order_count=self._out_of_order_count,
                malformed_count=self._malformed_count,
                reconnect_count=self._reconnect_count,
                timeout_count=self._timeout_count,
                callback_error_count=self._callback_error_count,
                last_record_age=age,
                last_error=self._last_error,
                ft_stall_count=self._ft_stall_count,
                ft_backward_count=self._ft_backward_count,
                ft_restart_count=self._ft_restart_count,
                last_ft_progress=self._last_ft_progress,
            )

    def start(self, sample_callback):
        if not callable(sample_callback):
            raise TypeError("sample_callback must be callable")
        with self._lock:
            if self._thread is not None:
                raise RuntimeError("NetFTClient is already running")
            self._callback = sample_callback
            stop_event = threading.Event()
            self._stop_event = stop_event
            self._state = ClientState.CONNECTING
            self._last_publish_monotonic = None
            self._reconnect_delay = float(self.config.reconnect_initial_delay)
            self._thread = threading.Thread(
                target=self._run,
                args=(stop_event,),
                name="netft-receiver",
                daemon=True,
            )
            self._thread.start()

    def _send_command_locked(self, command):
        if self._socket is None:
            raise NotConnectedError("Net F/T socket is not connected")
        self._socket.send(encode_request(command, 0))

    def _close_socket_locked(self, sock, send_stop):
        if sock is None:
            return
        owns_socket = self._socket is sock
        if owns_socket:
            self._socket = None
        if send_stop and owns_socket:
            try:
                sock.send(encode_request(Command.STOP_STREAMING, 0))
            except OSError:
                pass
        try:
            sock.close()
        except OSError:
            pass

    def _handle_record(self, record, now):
        with self._lock:
            self._state = ClientState.STREAMING
            self._last_valid_monotonic = now
            self._last_rdt_sequence = record.rdt_sequence
            self._last_ft_sequence = record.ft_sequence
            self._last_status = record.status
            self._received_count += 1
            self._reconnect_delay = float(self.config.reconnect_initial_delay)
            self._receive_times.append(now)
            self._prune_rate_window(self._receive_times, now)
            severity = classify_status(record.status)
            if severity is DiagnosticSeverity.ERROR:
                self._device_error_count += 1
            observation = self._sequence_tracker.observe(record.rdt_sequence)
            if observation.kind is SequenceKind.GAP:
                self._lost_count += observation.gap
            elif observation.kind is SequenceKind.DUPLICATE:
                self._duplicate_count += 1
                return
            elif observation.kind is SequenceKind.OUT_OF_ORDER:
                self._out_of_order_count += 1
                return
            ft_observation = self._ft_sequence_tracker.observe(record.ft_sequence)
            self._last_ft_progress = ft_observation.kind.value
            if ft_observation.kind is FtSequenceKind.STALL:
                self._ft_stall_count += 1
            elif ft_observation.kind is FtSequenceKind.BACKWARD:
                self._ft_backward_count += 1
            elif ft_observation.kind is FtSequenceKind.RESTART:
                self._ft_restart_count += 1
            if severity is DiagnosticSeverity.ERROR:
                if not self.config.publish_on_error:
                    return
            if self.config.publish_rate > 0.0:
                interval = 1.0 / float(self.config.publish_rate)
                if (
                    self._last_publish_monotonic is not None
                    and now - self._last_publish_monotonic < interval
                ):
                    self._rate_dropped_count += 1
                    return
            sample = self._convert_record(record, now)
            callback = self._callback
        try:
            callback(sample)
        except Exception as exc:
            with self._lock:
                self._callback_error_count += 1
                self._last_error = "sample callback failed: {}".format(exc)
            return
        with self._lock:
            self._published_count += 1
            self._publish_times.append(now)
            self._prune_rate_window(self._publish_times, now)
            self._last_publish_monotonic = now

    def _receive_session(self, stop_event):
        sock = None
        started = False
        try:
            receive_timeout = float(self.config.receive_timeout)
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.settimeout(min(_RECEIVE_POLL_INTERVAL, receive_timeout))
            sock.connect((self.config.sensor_host, self.config.sensor_port))
            with self._lock:
                if stop_event.is_set():
                    return
                self._socket = sock
                self._state = ClientState.CONNECTING
                self._sequence_tracker.reset()
                self._ft_sequence_tracker.begin_session()
                self._send_command_locked(Command.START_REALTIME)
                started = True
            last_valid = time.monotonic()
            while not stop_event.is_set():
                remaining = last_valid + receive_timeout - time.monotonic()
                if remaining <= 0.0:
                    raise socket.timeout("no valid RDT record before timeout")
                sock.settimeout(min(_RECEIVE_POLL_INTERVAL, remaining))
                try:
                    datagram = sock.recv(65535)
                except socket.timeout:
                    if stop_event.is_set():
                        return
                    if time.monotonic() >= last_valid + receive_timeout:
                        raise socket.timeout(
                            "no valid RDT record before timeout"
                        )
                    continue
                if stop_event.is_set():
                    return
                now = time.monotonic()
                try:
                    record = decode_record(datagram)
                except ProtocolError as exc:
                    with self._lock:
                        self._malformed_count += 1
                        self._last_error = str(exc)
                    if now - last_valid >= float(self.config.receive_timeout):
                        raise socket.timeout("no valid RDT record before timeout")
                    continue
                last_valid = now
                self._handle_record(record, now)
        finally:
            with self._lock:
                self._close_socket_locked(sock, send_stop=started)

    def _run(self, stop_event):
        try:
            while not stop_event.is_set():
                try:
                    self._receive_session(stop_event)
                except socket.timeout as exc:
                    if stop_event.is_set():
                        break
                    with self._lock:
                        self._timeout_count += 1
                        self._last_error = str(exc) or "RDT receive timeout"
                except OSError as exc:
                    if stop_event.is_set():
                        break
                    with self._lock:
                        self._last_error = str(exc)
                if stop_event.is_set():
                    break
                with self._lock:
                    self._state = ClientState.BACKOFF
                    self._reconnect_count += 1
                    delay = self._reconnect_delay
                if stop_event.wait(delay):
                    break
                with self._lock:
                    self._reconnect_delay = min(
                        delay * 2.0, float(self.config.reconnect_max_delay)
                    )
        finally:
            with self._lock:
                self._close_socket_locked(self._socket, send_stop=True)
                if self._thread is threading.current_thread():
                    self._state = ClientState.STOPPED
                    self._thread = None

    def bias(self):
        with self._lock:
            if self._socket is None or self._state is not ClientState.STREAMING:
                raise NotConnectedError("Net F/T is not streaming")
            self._send_command_locked(Command.SET_SOFTWARE_BIAS)
            self._send_command_locked(Command.START_REALTIME)
            self._sequence_tracker.reset()
            self._ft_sequence_tracker.begin_session()

    def stop(self):
        with self._lock:
            self._stop_event.set()
            self._close_socket_locked(self._socket, send_stop=True)
            thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=max(1.0, float(self.config.receive_timeout) * 2.0))
        with self._lock:
            if self._thread is None:
                self._state = ClientState.STOPPED
