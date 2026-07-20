import argparse
import json
import math
import os
import threading
import time
from dataclasses import asdict, dataclass
from pathlib import Path
import sys
import tempfile
from typing import Optional, Sequence, Tuple

from .client import ClientConfig, NetFTClient


@dataclass(frozen=True)
class CheckResult:
    endpoint: str
    requested_duration_s: float
    elapsed_s: float
    sample_count: int
    receive_rate_hz: float
    device_status: int
    last_rdt_sequence: Optional[int]
    last_ft_sequence: Optional[int]
    lost_count: int
    duplicate_count: int
    out_of_order_count: int
    malformed_count: int
    timeout_count: int
    reconnect_count: int
    last_force: Optional[Tuple[float, float, float]]
    last_torque: Optional[Tuple[float, float, float]]

    def json_dict(self):
        values = asdict(self)
        values["device_status"] = "0x{:08x}".format(self.device_status)
        return values


class _LastSampleSlot:
    def __init__(self):
        self._lock = threading.Lock()
        self._sample = None

    def store(self, sample):
        with self._lock:
            self._sample = sample

    def load(self):
        with self._lock:
            return self._sample


def _positive_finite_duration(duration):
    if not isinstance(duration, (int, float)) or isinstance(duration, bool):
        raise ValueError("duration must be finite and greater than zero")
    duration = float(duration)
    if not math.isfinite(duration) or duration <= 0.0:
        raise ValueError("duration must be finite and greater than zero")
    return duration


def _wait_for_first_record(client, timeout):
    deadline = time.monotonic() + timeout
    wait_event = threading.Event()
    while True:
        if client.health_snapshot().received_count:
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            return False
        wait_event.wait(min(0.01, remaining))


def run_check(config, duration):
    duration = _positive_finite_duration(duration)
    last_sample = _LastSampleSlot()

    client = NetFTClient(config)
    started = time.monotonic()
    try:
        client.start(last_sample.store)
        if not _wait_for_first_record(
            client, max(1.0, config.receive_timeout * 5.0)
        ):
            raise RuntimeError("no Net F/T sample received")
        remaining = duration - (time.monotonic() - started)
        if remaining > 0.0:
            time.sleep(remaining)
    finally:
        client.stop()
    elapsed = time.monotonic() - started
    health = client.health_snapshot()
    last = last_sample.load()
    return CheckResult(
        endpoint="{}:{}".format(config.sensor_host, config.sensor_port),
        requested_duration_s=duration,
        elapsed_s=elapsed,
        sample_count=health.received_count,
        receive_rate_hz=health.received_count / elapsed,
        device_status=health.last_status,
        last_rdt_sequence=health.last_rdt_sequence,
        last_ft_sequence=health.last_ft_sequence,
        lost_count=health.lost_count,
        duplicate_count=health.duplicate_count,
        out_of_order_count=health.out_of_order_count,
        malformed_count=health.malformed_count,
        timeout_count=health.timeout_count,
        reconnect_count=health.reconnect_count,
        last_force=None if last is None else last.force,
        last_torque=None if last is None else last.torque,
    )


def _parser():
    parser = argparse.ArgumentParser(description="Run a bounded ATI Net F/T RDT check")
    parser.add_argument("--host", default="192.168.31.100")
    parser.add_argument("--port", type=int, default=49152)
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--counts-per-force", type=float, default=1_000_000.0)
    parser.add_argument("--counts-per-torque", type=float, default=1_000_000.0)
    parser.add_argument("--output")
    return parser


def _write_output(path, text):
    output = Path(path)
    temporary = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".{}.".format(output.name), suffix=".tmp", dir=str(output.parent)
        )
        temporary = Path(temporary_name)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(text + "\n")
        os.replace(str(temporary), str(output))
        temporary = None
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass


def main(argv=None):
    try:
        arguments = _parser().parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 2
    try:
        config = ClientConfig(
            sensor_host=arguments.host,
            sensor_port=arguments.port,
            counts_per_force=arguments.counts_per_force,
            counts_per_torque=arguments.counts_per_torque,
        )
        result = run_check(config, arguments.duration)
        text = json.dumps(result.json_dict(), indent=2, sort_keys=True, allow_nan=False)
        if arguments.output:
            _write_output(arguments.output, text)
    except (OSError, OverflowError, RuntimeError, TypeError, ValueError) as exc:
        print("Net F/T check failed: {}".format(exc), file=sys.stderr)
        return 1
    print(text)
    return 0 if result.device_status == 0 and result.sample_count > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
