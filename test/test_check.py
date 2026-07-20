import json
import threading

import pytest

from netft_driver import check
from netft_driver.check import _LastSampleSlot, main, run_check
from netft_driver.client import ClientConfig
from netft_driver.protocol import Command
from test.support.fake_sensor import FakeNetFTSensor


def config_for(sensor, **overrides):
    values = {
        "sensor_host": sensor.host,
        "sensor_port": sensor.port,
        "receive_timeout": 0.05,
        "reconnect_initial_delay": 0.01,
        "reconnect_max_delay": 0.02,
    }
    values.update(overrides)
    return ClientConfig(**values)


def reject_non_json_constant(value):
    raise AssertionError("non-standard JSON constant: {}".format(value))


def assert_safe_shutdown(sensor, exact=False):
    commands = sensor.commands
    assert commands[-1] is Command.STOP_STREAMING
    assert Command.SET_SOFTWARE_BIAS not in commands
    assert Command.RESET_CONDITION_LATCH not in commands
    assert Command.START_BUFFERED not in commands
    if exact:
        assert commands == (Command.START_REALTIME, Command.STOP_STREAMING)


def test_run_check_returns_bounded_stream_summary_and_stops_cleanly():
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        result = run_check(config_for(sensor), duration=0.15)
        assert_safe_shutdown(sensor, exact=True)
    assert result.sample_count >= 10
    assert result.device_status == 0
    assert result.receive_rate_hz > 50.0
    assert result.lost_count == 0
    assert result.last_force == (0.0001, -0.0002, 0.0003)
    assert result.last_torque == (0.00001, -0.00002, 0.00003)


def test_last_sample_slot_replaces_samples_without_retaining_history():
    slot = _LastSampleSlot()
    for sample in range(10_000):
        slot.store(sample)
    assert slot.load() == 9_999
    assert not any(isinstance(value, list) for value in vars(slot).values())


def test_last_sample_slot_is_safe_for_concurrent_callback_and_reader():
    slot = _LastSampleSlot()
    stop = threading.Event()
    observed = []

    def read_samples():
        while not stop.is_set():
            observed.append(slot.load())

    reader = threading.Thread(target=read_samples)
    reader.start()
    try:
        for sample in range(1000):
            slot.store(sample)
    finally:
        stop.set()
        reader.join(timeout=1.0)
    slot.store("final")
    assert slot.load() == "final"
    assert not reader.is_alive()


def test_check_cli_writes_strict_json_with_independent_scales_and_stops(
    tmp_path, capsys
):
    output = tmp_path / "check.json"
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        return_code = main(
            [
                "--host",
                sensor.host,
                "--port",
                str(sensor.port),
                "--duration",
                "0.15",
                "--counts-per-force",
                "100",
                "--counts-per-torque",
                "10",
                "--output",
                str(output),
            ]
        )
        assert_safe_shutdown(sensor, exact=True)
    captured = capsys.readouterr()
    payload = json.loads(
        output.read_text(encoding="utf-8"), parse_constant=reject_non_json_constant
    )
    assert return_code == 0
    assert json.loads(captured.out, parse_constant=reject_non_json_constant) == payload
    assert captured.err == ""
    assert payload["sample_count"] >= 10
    assert payload["device_status"] == "0x00000000"
    assert payload["last_force"] == [1.0, -2.0, 3.0]
    assert payload["last_torque"] == [1.0, -2.0, 3.0]


@pytest.mark.parametrize(
    "duration",
    [0.0, -0.1, float("nan"), float("inf"), float("-inf")],
)
def test_run_check_rejects_non_positive_or_non_finite_duration_before_start(duration):
    with FakeNetFTSensor() as sensor:
        with pytest.raises(ValueError, match="finite and greater than zero"):
            run_check(config_for(sensor), duration)
        assert sensor.commands == ()


@pytest.mark.parametrize("duration", ["0", "-1", "nan", "inf", "-inf"])
def test_cli_rejects_invalid_duration_without_output_or_stream(
    duration, tmp_path, capsys
):
    output = tmp_path / "invalid.json"
    duration_arguments = (
        ["--duration=-inf"] if duration == "-inf" else ["--duration", duration]
    )
    with FakeNetFTSensor() as sensor:
        return_code = main(
            [
                "--host",
                sensor.host,
                "--port",
                str(sensor.port),
            ]
            + duration_arguments
            + [
                "--output",
                str(output),
            ]
        )
        assert sensor.commands == ()
    captured = capsys.readouterr()
    assert return_code != 0
    assert captured.out == ""
    assert "duration must be finite and greater than zero" in captured.err
    assert not output.exists()


def test_serious_record_returns_health_evidence_without_publishing_wrench(
    tmp_path, capsys
):
    output = tmp_path / "serious.json"
    with FakeNetFTSensor(rate_hz=1.0) as sensor:
        sensor.queue_record(55, status=0x80020000)
        return_code = main(
            [
                "--host",
                sensor.host,
                "--port",
                str(sensor.port),
                "--duration",
                "0.15",
                "--output",
                str(output),
            ]
        )
        assert_safe_shutdown(sensor)
    captured = capsys.readouterr()
    payload = json.loads(
        output.read_text(encoding="utf-8"), parse_constant=reject_non_json_constant
    )
    assert return_code == 1
    assert captured.err == ""
    assert payload["device_status"] == "0x80020000"
    assert payload["sample_count"] == 1
    assert payload["last_rdt_sequence"] == 55
    assert payload["last_ft_sequence"] == 1000
    assert payload["last_force"] is None
    assert payload["last_torque"] is None


def test_warning_record_is_received_and_returns_nonzero_json(tmp_path, capsys):
    output = tmp_path / "warning.json"
    with FakeNetFTSensor(rate_hz=1.0) as sensor:
        sensor.queue_record(31, status=0x80010000)
        return_code = main(
            [
                "--host",
                sensor.host,
                "--port",
                str(sensor.port),
                "--duration",
                "0.15",
                "--output",
                str(output),
            ]
        )
        assert_safe_shutdown(sensor)
    captured = capsys.readouterr()
    payload = json.loads(output.read_text(encoding="utf-8"))
    assert return_code == 1
    assert captured.err == ""
    assert payload["device_status"] == "0x80010000"
    assert payload["sample_count"] == 1
    assert payload["last_force"] == [0.0001, -0.0002, 0.0003]


def test_malformed_only_stream_fails_with_safe_shutdown():
    with FakeNetFTSensor(rate_hz=0.5) as sensor:
        sensor.queue_payload(b"malformed")
        with pytest.raises(RuntimeError, match="no Net F/T sample received"):
            run_check(config_for(sensor), duration=0.1)
        assert_safe_shutdown(sensor)


def test_silent_stream_fails_with_safe_shutdown():
    with FakeNetFTSensor() as sensor:
        sensor.pause()
        with pytest.raises(RuntimeError, match="no Net F/T sample received"):
            run_check(config_for(sensor), duration=0.1)
        assert_safe_shutdown(sensor)


def test_interruption_stops_stream_without_bias(monkeypatch):
    def interrupt(_duration):
        raise KeyboardInterrupt()

    monkeypatch.setattr(check.time, "sleep", interrupt)
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        with pytest.raises(KeyboardInterrupt):
            run_check(config_for(sensor), duration=0.15)
        assert_safe_shutdown(sensor, exact=True)


@pytest.mark.parametrize(
    "arguments, expected",
    [
        (["--host", " "], "sensor_host must be non-empty"),
        (["--port", "0"], "sensor_port must be between 1 and 65535"),
        (["--counts-per-force", "0"], "counts_per_force must be finite"),
    ],
)
def test_cli_configuration_errors_return_integer_and_stderr(arguments, expected, capsys):
    return_code = main(arguments)
    captured = capsys.readouterr()
    assert return_code != 0
    assert captured.out == ""
    assert expected in captured.err


def test_cli_argument_error_returns_integer_and_stderr(capsys):
    return_code = main(["--port", "not-a-port"])
    captured = capsys.readouterr()
    assert return_code == 2
    assert captured.out == ""
    assert "invalid int value" in captured.err


def test_cli_failed_output_returns_error_without_partial_json(tmp_path, capsys):
    output = tmp_path / "existing-directory"
    output.mkdir()
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        return_code = main(
            [
                "--host",
                sensor.host,
                "--port",
                str(sensor.port),
                "--duration",
                "0.15",
                "--output",
                str(output),
            ]
        )
        assert_safe_shutdown(sensor, exact=True)
    captured = capsys.readouterr()
    assert return_code != 0
    assert captured.out == ""
    assert "Net F/T check failed:" in captured.err
    assert output.is_dir()
