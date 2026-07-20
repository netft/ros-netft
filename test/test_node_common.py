from pathlib import Path

import pytest

from netft_driver.client import ClientState, HealthSnapshot
from netft_driver.node_common import (
    DIAGNOSTIC_VALUE_KEYS,
    MALFORMED_STORM_THRESHOLD,
    DiagnosticEvaluator,
    DiagnosticReport,
    FaultLogThrottle,
    build_diagnostic_values,
    log_diagnostic_fault,
    node_config_from_mapping,
)


def test_default_node_config_matches_real_sensor_profile():
    config = node_config_from_mapping({})
    assert config.client.sensor_host == "192.168.31.100"
    assert config.client.sensor_port == 49152
    assert config.client.counts_per_force == 1_000_000.0
    assert config.client.counts_per_torque == 1_000_000.0
    assert config.frame_id == "netft_link"
    assert config.wrench_topic == "/netft/wrench"
    assert config.bias_service == "/netft/bias"
    assert config.diagnostics_rate == 1.0
    assert config.expected_rdt_rate == 2000.0
    assert config.rate_tolerance == 0.2


def test_node_config_maps_ros_parameter_names_to_client_config():
    config = node_config_from_mapping(
        {
            "sensor_ip": "10.0.0.2",
            "sensor_port": 50000,
            "publish_rate": 250.0,
            "publish_on_error": True,
            "frame_id": "tool_sensor",
        }
    )
    assert config.client.sensor_host == "10.0.0.2"
    assert config.client.sensor_port == 50000
    assert config.client.publish_rate == 250.0
    assert config.client.publish_on_error is True
    assert config.frame_id == "tool_sensor"


@pytest.mark.parametrize(
    "values,match",
    [
        ({"frame_id": ""}, "frame_id"),
        ({"wrench_topic": ""}, "wrench_topic"),
        ({"bias_service": ""}, "bias_service"),
        ({"diagnostics_rate": 0.0}, "diagnostics_rate"),
        ({"expected_rdt_rate": 0.0}, "expected_rdt_rate"),
        ({"rate_tolerance": -0.1}, "rate_tolerance"),
        ({"rate_tolerance": 1.1}, "rate_tolerance"),
    ],
)
def test_node_config_rejects_invalid_ros_values(values, match):
    with pytest.raises((TypeError, ValueError), match=match):
        node_config_from_mapping(values)


def health(**overrides):
    values = {
        "state": ClientState.STREAMING,
        "sensor_host": "192.168.31.100",
        "sensor_port": 49152,
        "last_rdt_sequence": 100,
        "last_ft_sequence": 400,
        "last_status": 0,
        "receive_rate": 2000.0,
        "publish_rate": 2000.0,
        "received_count": 2000,
        "published_count": 2000,
        "rate_dropped_count": 0,
        "device_error_count": 0,
        "lost_count": 0,
        "duplicate_count": 0,
        "out_of_order_count": 0,
        "malformed_count": 0,
        "reconnect_count": 0,
        "timeout_count": 0,
        "callback_error_count": 0,
        "last_record_age": 0.001,
        "last_error": "",
    }
    values.update(overrides)
    return HealthSnapshot(**values)


def test_diagnostics_are_ok_for_healthy_in_tolerance_stream():
    report = DiagnosticEvaluator(2000.0, 0.2).evaluate(health())
    assert report.level == 0
    assert report.message == "streaming normally"
    assert ("device_status", "0x00000000") in report.values
    assert ("expected_receive_rate_hz", "2000.0") in report.values
    assert ("rate_tolerance", "0.200") in report.values


def test_diagnostic_key_contract_is_exact_and_shared_by_ros_adapters():
    report = DiagnosticEvaluator(2000.0, 0.2).evaluate(health())
    assert tuple(key for key, _value in report.values) == DIAGNOSTIC_VALUE_KEYS

    class Ros1KeyValue:
        def __init__(self, key, value):
            self.key = key
            self.value = value

    class Ros2KeyValue:
        def __init__(self, key, value):
            self.key = key
            self.value = value

    ros1_keys = tuple(
        item.key for item in build_diagnostic_values(report, Ros1KeyValue)
    )
    ros2_keys = tuple(
        item.key for item in build_diagnostic_values(report, Ros2KeyValue)
    )
    assert ros1_keys == ros2_keys == DIAGNOSTIC_VALUE_KEYS

    root = Path(__file__).resolve().parents[1]
    for adapter in ("ros1_node.py", "ros2_node.py"):
        source = root.joinpath("netft_driver", adapter).read_text(encoding="utf-8")
        assert "build_diagnostic_values(report, KeyValue)" in source


def test_diagnostics_warn_once_for_new_packet_loss():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    first = evaluator.evaluate(health(lost_count=3))
    second = evaluator.evaluate(health(lost_count=3))
    assert first.level == 1
    assert "3 RDT records lost" in first.message
    assert second.level == 0


def test_diagnostics_warn_for_condition_latch_and_rate_deviation():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    condition = evaluator.evaluate(health(last_status=0x80010000))
    slow = evaluator.evaluate(health(receive_rate=1000.0))
    assert condition.level == 1
    assert "monitor condition latched" in condition.message
    assert slow.level == 1
    assert "receive rate" in slow.message


def test_diagnostics_error_for_device_fault_and_backoff():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    device = evaluator.evaluate(health(last_status=0x80020000))
    backoff = evaluator.evaluate(health(state=ClientState.BACKOFF))
    assert device.level == 2
    assert "transducer saturation" in device.message
    assert backoff.level == 2
    assert "reconnecting" in backoff.message


def test_recovered_serious_status_is_latched_for_one_diagnostic_cycle():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    assert evaluator.evaluate(health()).level == 0
    recovered = evaluator.evaluate(
        health(device_error_count=1, last_status=0, last_error="")
    )
    settled = evaluator.evaluate(
        health(device_error_count=1, last_status=0, last_error="")
    )
    assert recovered.level == 2
    assert "serious device status" in recovered.message
    assert settled.level == 0


def test_recovered_receive_timeout_is_latched_for_one_diagnostic_cycle():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    assert evaluator.evaluate(health()).level == 0
    recovered = evaluator.evaluate(
        health(timeout_count=1, reconnect_count=1, last_error="")
    )
    settled = evaluator.evaluate(
        health(timeout_count=1, reconnect_count=1, last_error="")
    )
    assert recovered.level == 2
    assert "receive timeout" in recovered.message
    assert settled.level == 0


def test_timeout_event_takes_priority_over_reconnecting_state_once():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    timed_out = evaluator.evaluate(
        health(state=ClientState.CONNECTING, timeout_count=1, reconnect_count=1)
    )
    still_connecting = evaluator.evaluate(
        health(state=ClientState.CONNECTING, timeout_count=1, reconnect_count=1)
    )
    assert timed_out.level == 2
    assert "receive timeout" in timed_out.message
    assert still_connecting.level == 1
    assert "waiting for first" in still_connecting.message


def test_malformed_packet_storm_uses_one_diagnostic_window_and_exact_threshold():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    below = evaluator.evaluate(
        health(malformed_count=MALFORMED_STORM_THRESHOLD - 1)
    )
    storm = evaluator.evaluate(
        health(malformed_count=(2 * MALFORMED_STORM_THRESHOLD) - 1)
    )
    settled = evaluator.evaluate(
        health(malformed_count=(2 * MALFORMED_STORM_THRESHOLD) - 1)
    )
    assert below.level == 0
    assert storm.level == 2
    assert "malformed-packet storm" in storm.message
    assert settled.level == 0


def test_ft_progress_faults_and_restart_are_visible_in_diagnostics_once():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    evaluator.evaluate(health())
    stalled = evaluator.evaluate(
        health(ft_stall_count=1, last_ft_progress="forward")
    )
    backward = evaluator.evaluate(
        health(
            ft_stall_count=1,
            ft_backward_count=1,
            last_ft_progress="forward",
        )
    )
    restarted = evaluator.evaluate(
        health(
            ft_stall_count=1,
            ft_backward_count=1,
            ft_restart_count=1,
            last_ft_progress="forward",
        )
    )
    settled = evaluator.evaluate(
        health(
            ft_stall_count=1,
            ft_backward_count=1,
            ft_restart_count=1,
            last_ft_progress="forward",
        )
    )
    assert stalled.level == 2
    assert "stalled" in stalled.message
    assert backward.level == 2
    assert "backward" in backward.message
    assert restarted.level == 1
    assert "restarted" in restarted.message
    assert settled.level == 0


def test_backoff_is_immediately_error_without_counter_change():
    evaluator = DiagnosticEvaluator(2000.0, 0.2)
    evaluator.evaluate(health())
    assert evaluator.evaluate(health(state=ClientState.BACKOFF)).level == 2


def test_fault_logging_is_immediate_on_transition_and_periodic_when_persistent():
    throttle = FaultLogThrottle(repeat_interval=10.0)
    warning = DiagnosticReport(1, "slow", (), "receive_rate")
    error = DiagnosticReport(2, "lost", (), "backoff")
    healthy = DiagnosticReport(0, "normal", (), "healthy")

    assert throttle.should_log(warning, now=1.0)
    assert not throttle.should_log(warning, now=2.0)
    assert throttle.should_log(warning, now=11.0)
    assert throttle.should_log(error, now=11.1)
    assert not throttle.should_log(error, now=12.0)
    assert not throttle.should_log(healthy, now=12.1)
    assert throttle.should_log(error, now=12.2)


def test_fault_log_adapter_uses_native_warning_and_error_severities():
    throttle = FaultLogThrottle(repeat_interval=10.0)
    warnings = []
    errors = []
    assert log_diagnostic_fault(
        DiagnosticReport(1, "slow", (), "receive_rate"),
        throttle,
        warnings.append,
        errors.append,
        now=1.0,
    )
    assert log_diagnostic_fault(
        DiagnosticReport(2, "lost", (), "backoff"),
        throttle,
        warnings.append,
        errors.append,
        now=1.1,
    )
    assert warnings == ["slow"]
    assert errors == ["lost"]


def test_diagnostic_evaluator_rejects_boolean_rate_tolerance():
    with pytest.raises(ValueError, match="rate_tolerance"):
        DiagnosticEvaluator(2000.0, True)
