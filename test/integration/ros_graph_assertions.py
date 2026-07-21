#!/usr/bin/env python3
import argparse
import re
from pathlib import Path


def _unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def _timestamp(text, ros_version):
    seconds_field, nanoseconds_field = (
        ("secs", "nsecs") if ros_version == 1 else ("sec", "nanosec")
    )
    seconds = re.search(rf"^\s*{seconds_field}:\s*(\d+)\s*$", text, re.MULTILINE)
    nanoseconds = re.search(
        rf"^\s*{nanoseconds_field}:\s*(\d+)\s*$", text, re.MULTILINE
    )
    assert seconds and nanoseconds, "message stamp fields are missing"
    stamp = int(seconds.group(1)) + int(nanoseconds.group(1)) / 1_000_000_000
    assert stamp > 0.0, "message stamp must be valid and non-zero"
    return stamp


def assert_wrench_output(path, *, ros_version, frame_id, axes):
    text = Path(path).read_text(encoding="utf-8")
    _timestamp(text, ros_version)
    frame_match = re.search(r"^\s*frame_id:\s*(.*?)\s*$", text, re.MULTILINE)
    assert frame_match, "wrench frame_id is missing"
    assert _unquote(frame_match.group(1)) == frame_id, frame_match.group(1)

    wrench = text.split("wrench:", 1)
    assert len(wrench) == 2, "wrench body is missing"
    observed = [
        float(value)
        for value in re.findall(
            r"^\s*[xyz]:\s*([-+0-9.eE]+)\s*$", wrench[1], re.MULTILINE
        )
    ]
    assert len(observed) == 6, f"expected six independent wrench axes, got {observed}"
    assert observed == list(axes), f"wrench axes {observed} != {list(axes)}"


def _core_diagnostic_keys(status_header):
    text = Path(status_header).read_text(encoding="utf-8")
    match = re.search(r"kDiagnosticValueKeys\s*\{(.*?)\};", text, re.DOTALL)
    assert match, "kDiagnosticValueKeys is missing from the core status header"
    return re.findall(r'"([^"]+)"', match.group(1))


def _diagnostic_pairs(message):
    pairs = []
    lines = message.splitlines()
    for index, line in enumerate(lines):
        key = re.match(r"^\s*-?\s*key:\s*(.*?)\s*$", line)
        if not key:
            continue
        assert index + 1 < len(lines), f"diagnostic value missing after {line}"
        value = re.match(r"^\s*value:\s*(.*?)\s*$", lines[index + 1])
        assert value, f"diagnostic value missing after {line}"
        pairs.append((_unquote(key.group(1)), _unquote(value.group(1))))
    return pairs


def assert_diagnostics_output(
    path,
    *,
    ros_version,
    status_header,
    expected_rate,
    expected_values,
    configured_publish_rate=None,
):
    text = Path(path).read_text(encoding="utf-8")
    messages = [part for part in re.split(r"^---\s*$", text, flags=re.MULTILINE) if "key:" in part]
    assert len(messages) >= 5, f"expected at least five diagnostics messages, got {len(messages)}"
    expected_keys = _core_diagnostic_keys(status_header)
    stamps = []
    for message in messages:
        stamps.append(_timestamp(message, ros_version))
        pairs = _diagnostic_pairs(message)
        observed_keys = [key for key, _ in pairs]
        assert observed_keys == expected_keys, (
            f"diagnostic keys {observed_keys} != core keys {expected_keys}"
        )
        values = dict(pairs)
        for key, expected in expected_values.items():
            assert values.get(key) == expected, f"{key}={values.get(key)!r}, expected {expected!r}"

    elapsed = stamps[-1] - stamps[0]
    assert elapsed > 0.0, f"diagnostic stamps are not increasing: {stamps}"
    observed_rate = (len(stamps) - 1) / elapsed
    assert expected_rate * 0.6 <= observed_rate <= expected_rate * 1.6, (
        f"diagnostics rate {observed_rate:.3f} Hz outside tolerant window for {expected_rate} Hz"
    )
    if configured_publish_rate is not None:
        values = dict(_diagnostic_pairs(messages[-1]))
        publish_rate = float(values["publish_rate_hz"])
        receive_rate = float(values["receive_rate_hz"])
        dropped = int(values["rate_dropped_count"])
        assert configured_publish_rate * 0.35 <= publish_rate <= configured_publish_rate * 1.4
        assert publish_rate < receive_rate
        assert dropped > 0


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)
    wrench = subparsers.add_parser("wrench")
    wrench.add_argument("path", type=Path)
    wrench.add_argument("--ros-version", type=int, choices=(1, 2), required=True)
    wrench.add_argument("--frame-id", required=True)
    wrench.add_argument("--axes", type=float, nargs=6, required=True)
    diagnostics = subparsers.add_parser("diagnostics")
    diagnostics.add_argument("path", type=Path)
    diagnostics.add_argument("--ros-version", type=int, choices=(1, 2), required=True)
    diagnostics.add_argument("--status-header", type=Path, required=True)
    diagnostics.add_argument("--expected-rate", type=float, required=True)
    diagnostics.add_argument("--expected-value", action="append", default=[])
    diagnostics.add_argument("--configured-publish-rate", type=float)
    arguments = parser.parse_args()

    if arguments.mode == "wrench":
        assert_wrench_output(
            arguments.path,
            ros_version=arguments.ros_version,
            frame_id=arguments.frame_id,
            axes=arguments.axes,
        )
    else:
        expected_values = dict(value.split("=", 1) for value in arguments.expected_value)
        assert_diagnostics_output(
            arguments.path,
            ros_version=arguments.ros_version,
            status_header=arguments.status_header,
            expected_rate=arguments.expected_rate,
            expected_values=expected_values,
            configured_publish_rate=arguments.configured_publish_rate,
        )


if __name__ == "__main__":
    main()
