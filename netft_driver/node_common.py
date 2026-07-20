import math
import time
from dataclasses import dataclass
from typing import Mapping, Tuple

from .client import ClientConfig, ClientState, HealthSnapshot
from .status import DiagnosticSeverity, classify_status, decode_status


DEFAULT_PARAMETERS = {
    "sensor_ip": "192.168.31.100",
    "sensor_port": 49152,
    "frame_id": "netft_link",
    "wrench_topic": "/netft/wrench",
    "bias_service": "/netft/bias",
    "counts_per_force": 1_000_000.0,
    "counts_per_torque": 1_000_000.0,
    "publish_rate": 0.0,
    "receive_timeout": 0.1,
    "reconnect_initial_delay": 0.25,
    "reconnect_max_delay": 5.0,
    "diagnostics_rate": 1.0,
    "expected_rdt_rate": 2000.0,
    "rate_tolerance": 0.2,
    "publish_on_error": False,
}

MALFORMED_STORM_THRESHOLD = 10
FAULT_LOG_REPEAT_INTERVAL = 10.0
DIAGNOSTIC_VALUE_KEYS = (
    "state",
    "sensor",
    "last_rdt_sequence",
    "last_ft_sequence",
    "last_ft_progress",
    "device_status",
    "active_status",
    "receive_rate_hz",
    "expected_receive_rate_hz",
    "rate_tolerance",
    "publish_rate_hz",
    "received_count",
    "published_count",
    "rate_dropped_count",
    "device_error_count",
    "lost_count",
    "duplicate_count",
    "out_of_order_count",
    "ft_stall_count",
    "ft_backward_count",
    "ft_restart_count",
    "malformed_count",
    "malformed_storm_threshold",
    "malformed_storm_window",
    "reconnect_count",
    "timeout_count",
    "callback_error_count",
    "last_record_age_s",
    "last_error",
)


@dataclass(frozen=True)
class NodeConfig:
    client: ClientConfig
    frame_id: str
    wrench_topic: str
    bias_service: str
    diagnostics_rate: float
    expected_rdt_rate: float
    rate_tolerance: float


@dataclass(frozen=True)
class DiagnosticReport:
    level: int
    message: str
    values: Tuple[Tuple[str, str], ...]
    log_key: str = ""


class FaultLogThrottle:
    def __init__(self, repeat_interval=FAULT_LOG_REPEAT_INTERVAL):
        self.repeat_interval = _finite_positive(
            "repeat_interval", repeat_interval
        )
        self._active_key = None
        self._last_log_monotonic = None

    def should_log(self, report, now=None):
        if report.level <= DiagnosticSeverity.OK:
            self._active_key = None
            self._last_log_monotonic = None
            return False
        if now is None:
            now = time.monotonic()
        key = (report.level, report.log_key or report.message)
        if (
            key != self._active_key
            or self._last_log_monotonic is None
            or now - self._last_log_monotonic >= self.repeat_interval
        ):
            self._active_key = key
            self._last_log_monotonic = now
            return True
        return False


def log_diagnostic_fault(
    report, throttle, warning_logger, error_logger, now=None
):
    if not throttle.should_log(report, now=now):
        return False
    if report.level >= DiagnosticSeverity.ERROR:
        error_logger(report.message)
    else:
        warning_logger(report.message)
    return True


def build_diagnostic_values(report, key_value_type):
    return [
        key_value_type(key=key, value=value) for key, value in report.values
    ]


def _finite_positive(name, value):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise TypeError("{} must be numeric".format(name))
    number = float(value)
    if not math.isfinite(number) or number <= 0.0:
        raise ValueError("{} must be finite and greater than zero".format(name))
    return number


def node_config_from_mapping(mapping):
    values = dict(DEFAULT_PARAMETERS)
    values.update(mapping)
    for name in ("frame_id", "wrench_topic", "bias_service"):
        if not isinstance(values[name], str) or not values[name].strip():
            raise ValueError("{} must be non-empty".format(name))
    diagnostics_rate = _finite_positive("diagnostics_rate", values["diagnostics_rate"])
    expected_rate = _finite_positive("expected_rdt_rate", values["expected_rdt_rate"])
    tolerance = values["rate_tolerance"]
    if (
        not isinstance(tolerance, (int, float))
        or isinstance(tolerance, bool)
        or not math.isfinite(float(tolerance))
        or not 0.0 <= float(tolerance) <= 1.0
    ):
        raise ValueError("rate_tolerance must be between 0 and 1")
    client = ClientConfig(
        sensor_host=values["sensor_ip"],
        sensor_port=values["sensor_port"],
        counts_per_force=values["counts_per_force"],
        counts_per_torque=values["counts_per_torque"],
        publish_rate=values["publish_rate"],
        receive_timeout=values["receive_timeout"],
        reconnect_initial_delay=values["reconnect_initial_delay"],
        reconnect_max_delay=values["reconnect_max_delay"],
        publish_on_error=values["publish_on_error"],
    )
    return NodeConfig(
        client=client,
        frame_id=values["frame_id"],
        wrench_topic=values["wrench_topic"],
        bias_service=values["bias_service"],
        diagnostics_rate=diagnostics_rate,
        expected_rdt_rate=expected_rate,
        rate_tolerance=float(tolerance),
    )


class DiagnosticEvaluator:
    def __init__(self, expected_rdt_rate, rate_tolerance):
        self.expected_rdt_rate = _finite_positive(
            "expected_rdt_rate", expected_rdt_rate
        )
        if (
            not isinstance(rate_tolerance, (int, float))
            or isinstance(rate_tolerance, bool)
            or not math.isfinite(float(rate_tolerance))
            or not 0.0 <= float(rate_tolerance) <= 1.0
        ):
            raise ValueError("rate_tolerance must be between 0 and 1")
        self.rate_tolerance = float(rate_tolerance)
        self._previous_counts = {
            "lost_count": 0,
            "device_error_count": 0,
            "timeout_count": 0,
            "malformed_count": 0,
            "ft_stall_count": 0,
            "ft_backward_count": 0,
            "ft_restart_count": 0,
        }

    def _value_pairs(self, snapshot):
        status_names = ", ".join(decode_status(snapshot.last_status))
        pairs = (
            ("state", snapshot.state.value),
            ("sensor", "{}:{}".format(snapshot.sensor_host, snapshot.sensor_port)),
            ("last_rdt_sequence", str(snapshot.last_rdt_sequence)),
            ("last_ft_sequence", str(snapshot.last_ft_sequence)),
            ("last_ft_progress", snapshot.last_ft_progress),
            ("device_status", "0x{:08x}".format(snapshot.last_status)),
            ("active_status", status_names),
            ("receive_rate_hz", "{:.1f}".format(snapshot.receive_rate)),
            (
                "expected_receive_rate_hz",
                "{:.1f}".format(self.expected_rdt_rate),
            ),
            ("rate_tolerance", "{:.3f}".format(self.rate_tolerance)),
            ("publish_rate_hz", "{:.1f}".format(snapshot.publish_rate)),
            ("received_count", str(snapshot.received_count)),
            ("published_count", str(snapshot.published_count)),
            ("rate_dropped_count", str(snapshot.rate_dropped_count)),
            ("device_error_count", str(snapshot.device_error_count)),
            ("lost_count", str(snapshot.lost_count)),
            ("duplicate_count", str(snapshot.duplicate_count)),
            ("out_of_order_count", str(snapshot.out_of_order_count)),
            ("ft_stall_count", str(snapshot.ft_stall_count)),
            ("ft_backward_count", str(snapshot.ft_backward_count)),
            ("ft_restart_count", str(snapshot.ft_restart_count)),
            ("malformed_count", str(snapshot.malformed_count)),
            ("malformed_storm_threshold", str(MALFORMED_STORM_THRESHOLD)),
            ("malformed_storm_window", "between_diagnostic_updates"),
            ("reconnect_count", str(snapshot.reconnect_count)),
            ("timeout_count", str(snapshot.timeout_count)),
            ("callback_error_count", str(snapshot.callback_error_count)),
            (
                "last_record_age_s",
                "none"
                if snapshot.last_record_age is None
                else "{:.3f}".format(snapshot.last_record_age),
            ),
            ("last_error", snapshot.last_error),
        )
        if tuple(key for key, _value in pairs) != DIAGNOSTIC_VALUE_KEYS:
            raise AssertionError("diagnostic key contract is inconsistent")
        return pairs

    def _counter_deltas(self, snapshot):
        deltas = {}
        for name, previous in self._previous_counts.items():
            current = getattr(snapshot, name)
            deltas[name] = max(0, current - previous)
            self._previous_counts[name] = current
        return deltas

    def evaluate(self, snapshot):
        values = self._value_pairs(snapshot)
        deltas = self._counter_deltas(snapshot)
        if snapshot.state is ClientState.BACKOFF:
            return DiagnosticReport(
                2, "connection lost; reconnecting", values, "backoff"
            )
        if snapshot.state is ClientState.STOPPED:
            return DiagnosticReport(2, "client stopped", values, "stopped")
        status_severity = classify_status(snapshot.last_status)
        if status_severity is DiagnosticSeverity.ERROR:
            names = ", ".join(decode_status(snapshot.last_status))
            return DiagnosticReport(
                2, "device error: {}".format(names), values, "device_error"
            )
        if deltas["device_error_count"]:
            return DiagnosticReport(
                2,
                "serious device status observed since last diagnostic",
                values,
                "device_error_event",
            )
        if deltas["timeout_count"]:
            return DiagnosticReport(
                2,
                "receive timeout observed since last diagnostic",
                values,
                "receive_timeout",
            )
        if deltas["malformed_count"] >= MALFORMED_STORM_THRESHOLD:
            return DiagnosticReport(
                2,
                "malformed-packet storm: {} packets since last diagnostic".format(
                    deltas["malformed_count"]
                ),
                values,
                "malformed_storm",
            )
        if deltas["ft_stall_count"]:
            return DiagnosticReport(
                2,
                "FT sequence stalled {} times since last diagnostic".format(
                    deltas["ft_stall_count"]
                ),
                values,
                "ft_stall",
            )
        if deltas["ft_backward_count"]:
            return DiagnosticReport(
                2,
                "FT sequence moved backward {} times since last diagnostic".format(
                    deltas["ft_backward_count"]
                ),
                values,
                "ft_backward",
            )
        if snapshot.state is ClientState.CONNECTING:
            return DiagnosticReport(
                1, "waiting for first RDT record", values, "connecting"
            )
        if status_severity is DiagnosticSeverity.WARN:
            return DiagnosticReport(
                1, "monitor condition latched", values, "condition_latch"
            )
        if deltas["ft_restart_count"]:
            return DiagnosticReport(
                1,
                "FT device counter restarted since last diagnostic",
                values,
                "ft_restart",
            )
        if deltas["lost_count"]:
            return DiagnosticReport(
                1,
                "{} RDT records lost".format(deltas["lost_count"]),
                values,
                "packet_loss",
            )
        lower = self.expected_rdt_rate * (1.0 - self.rate_tolerance)
        upper = self.expected_rdt_rate * (1.0 + self.rate_tolerance)
        if not lower <= snapshot.receive_rate <= upper:
            return DiagnosticReport(
                1,
                "receive rate {:.1f} Hz outside {:.1f}-{:.1f} Hz".format(
                    snapshot.receive_rate, lower, upper
                ),
                values,
                "receive_rate",
            )
        if snapshot.callback_error_count:
            return DiagnosticReport(
                1, "sample callback has failed", values, "callback_error"
            )
        return DiagnosticReport(0, "streaming normally", values, "healthy")
