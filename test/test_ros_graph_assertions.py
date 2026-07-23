import re
from pathlib import Path

import pytest

from test.integration.ros_graph_assertions import (
    assert_diagnostics_output,
    assert_wrench_output,
)


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    ("name", "prefix"), [("ros1_smoke.sh", "_"), ("ros2_smoke.sh", "-p ")]
)
def test_smokes_exercise_namespaced_remapped_graph_and_all_nondefault_parameters(
    name, prefix
):
    source = (ROOT / "test/integration" / name).read_text(encoding="utf-8")
    assert "wrench:=remapped_wrench" in source
    assert "/netft_smoke/remapped_wrench" in source
    assert "ros_graph_assertions.py\" wrench" in source
    assert "ros_graph_assertions.py\" diagnostics" in source
    values = {
        "sensor_ip": "127.0.0.1",
        "sensor_port": '"$sensor_port"',
        "http_port": '"$http_port"',
        "frame_id": "smoke_frame",
        "wrench_topic": "wrench",
        "bias_service": "bias",
        "use_sensor_calibration": "true",
        "publish_rate": "50.0",
        "receive_timeout": "0.8",
        "configuration_connect_timeout": "0.23",
        "configuration_timeout": "0.71",
        "reconnect_initial_delay": "0.11",
        "reconnect_max_delay": "0.37",
        "diagnostics_rate": "5.0",
        "expected_rdt_rate": "200.0",
        "rate_tolerance": "0.35",
        "publish_on_error": "true",
    }
    for key, value in values.items():
        assert f"{prefix}{key}:={value}" in source


@pytest.mark.parametrize(
    ("ros_version", "seconds_field", "nanoseconds_field", "quoted"),
    [(1, "secs", "nsecs", True), (2, "sec", "nanosec", False)],
)
def test_wrench_assertion_requires_valid_stamp_frame_and_independent_axes(
    tmp_path, ros_version, seconds_field, nanoseconds_field, quoted
):
    frame = '"smoke_frame"' if quoted else "smoke_frame"
    output = tmp_path / "wrench.txt"
    output.write_text(
        f"""header:
  stamp:
    {seconds_field}: 17
    {nanoseconds_field}: 250000000
  frame_id: {frame}
wrench:
  force:
    x: 100.0
    y: -200.0
    z: 300.0
  torque:
    x: 0.001
    y: -0.002
    z: 0.003
""",
        encoding="utf-8",
    )

    assert_wrench_output(
        output,
        ros_version=ros_version,
        frame_id="smoke_frame",
        axes=(100.0, -200.0, 300.0, 0.001, -0.002, 0.003),
    )

    output.write_text(
        output.read_text(encoding="utf-8")
        .replace(f"{seconds_field}: 17", f"{seconds_field}: 0", 1)
        .replace(f"{nanoseconds_field}: 250000000", f"{nanoseconds_field}: 0", 1),
        encoding="utf-8",
    )
    with pytest.raises(AssertionError):
        assert_wrench_output(
            output,
            ros_version=ros_version,
            frame_id="smoke_frame",
            axes=(100.0, -200.0, 300.0, 0.001, -0.002, 0.003),
        )


def test_diagnostics_assertion_matches_core_key_order_rate_and_values(tmp_path):
    status_header = ROOT / "src/ros/diagnostics.hpp"
    header_text = status_header.read_text(encoding="utf-8")
    key_block = header_text.split("kDiagnosticValueKeys{", 1)[1].split("};", 1)[0]
    keys = [part.split('"', 2)[1] for part in key_block.split(",")]
    messages = []
    for index in range(5):
        pairs = []
        for key in keys:
            value = {
                "expected_receive_rate_hz": "200.0",
                "rate_tolerance": "0.350",
                "delivery_rate_hz": "50.0",
            }.get(key, "fixture")
            pairs.append(f'- key: "{key}"\n  value: "{value}"')
        messages.append(
            "header:\n"
            "  stamp:\n"
            f"    secs: {100 + index // 5}\n"
            f"    nsecs: {(index % 5) * 200000000}\n"
            "status:\n"
            "- name: \"netft_driver: connection\"\n"
            + "\n".join(pairs)
        )
    output = tmp_path / "diagnostics.txt"
    output.write_text("\n---\n".join(messages), encoding="utf-8")

    assert_diagnostics_output(
        output,
        ros_version=1,
        status_header=status_header,
        expected_rate=5.0,
        expected_values={
            "expected_receive_rate_hz": "200.0",
            "rate_tolerance": "0.350",
            "delivery_rate_hz": "50.0",
        },
    )

    valid_output = output.read_text(encoding="utf-8")
    output.write_text(
        valid_output.replace(
            '- key: "state"\n  value: "fixture"\n- key: "sensor"',
            '- key: "sensor"\n  value: "fixture"\n- key: "state"',
            1,
        ),
        encoding="utf-8",
    )
    with pytest.raises(AssertionError):
        assert_diagnostics_output(
            output,
            ros_version=1,
            status_header=status_header,
            expected_rate=5.0,
            expected_values={},
        )

    output.write_text(
        re.sub(
            r"(nsecs: )(\d+)",
            lambda match: match.group(1) + str(int(match.group(2)) // 2),
            valid_output,
        ),
        encoding="utf-8",
    )
    with pytest.raises(AssertionError):
        assert_diagnostics_output(
            output,
            ros_version=1,
            status_header=status_header,
            expected_rate=5.0,
            expected_values={},
        )
