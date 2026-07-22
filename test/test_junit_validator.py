from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "test/integration/validate_junit.py"


def _junit_xml(outcomes):
    cases = []
    for index, outcome in enumerate(outcomes):
        child = "" if outcome == "passed" else "<{} />".format(outcome)
        cases.append(
            '<testcase classname="fixture" name="case_{}">{}</testcase>'.format(
                index, child
            )
        )
    counts = {
        outcome: outcomes.count(outcome)
        for outcome in ("skipped", "failure", "error")
    }
    return (
        '<testsuite name="fixture" tests="{}" failures="{}" errors="{}" '
        'skipped="{}">{}</testsuite>\n'.format(
            len(outcomes),
            counts["failure"],
            counts["error"],
            counts["skipped"],
            "".join(cases),
        )
    )


def _run_validator(tmp_path, target, expectation, files):
    results_root = tmp_path / "results"
    for relative_path, xml in files.items():
        path = results_root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(xml, encoding="utf-8")
    return subprocess.run(
        ["python3", str(VALIDATOR), str(results_root), target, expectation],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        check=False,
    )


@pytest.mark.parametrize(
    ("target", "expectation", "relative_path", "outcomes"),
    [
        (
            "netft_unit",
            "at-least-one",
            "catkin/netft_driver/pytest-netft_unit.xml",
            ["passed", "skipped"],
        ),
        (
            "netft_ros2_smoke_harness",
            "exactly-one",
            "ament/netft_driver/netft_ros2_smoke_harness.xunit.xml",
            ["passed"],
        ),
        (
            "netft_ros1_node",
            "at-least-one",
            "catkin/netft_driver/gtest-netft_ros1_node.xml",
            ["passed", "passed"],
        ),
        (
            "netft_hardware_interface",
            "at-least-one",
            "ament/netft_driver/netft_hardware_interface.gtest.xml",
            ["passed", "passed", "passed"],
        ),
    ],
)
def test_validator_accepts_expected_non_skipped_counts(
    tmp_path, target, expectation, relative_path, outcomes
):
    completed = _run_validator(
        tmp_path, target, expectation, {relative_path: _junit_xml(outcomes)}
    )

    assert completed.returncode == 0, completed.stdout


@pytest.mark.parametrize("outcomes", [[], ["skipped", "skipped"]])
def test_unit_validator_rejects_zero_non_skipped_tests(tmp_path, outcomes):
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"pytest-netft_unit.xml": _junit_xml(outcomes)},
    )

    assert completed.returncode != 0


def test_harness_validator_rejects_the_wrong_executed_count(tmp_path):
    completed = _run_validator(
        tmp_path,
        "netft_ros1_smoke_harness",
        "exactly-one",
        {
            "pytest-netft_ros1_smoke_harness.xml": _junit_xml(
                ["passed", "passed"]
            )
        },
    )

    assert completed.returncode != 0


@pytest.mark.parametrize("outcome", ["failure", "error"])
def test_validator_rejects_failures_and_errors(tmp_path, outcome):
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"pytest-netft_unit.xml": _junit_xml([outcome])},
    )

    assert completed.returncode != 0


def test_validator_rejects_missing_or_ambiguous_target_results(tmp_path):
    missing = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"unrelated.xml": _junit_xml(["passed"])},
    )
    assert missing.returncode != 0

    ambiguous = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {
            "one/pytest-netft_unit.xml": _junit_xml(["passed"]),
            "two/netft_unit.xunit.xml": _junit_xml(["passed"]),
        },
    )
    assert ambiguous.returncode != 0


def test_summary_accepts_reconciled_plural_and_nested_aggregates(tmp_path):
    xml = """\
<testsuites name="all" tests="3" skipped="1" failures="0" errors="0">
  <testsuite name="group" tests="3" skipped="1" failures="0" errors="0">
    <testsuite name="first" tests="1" skipped="0" failures="0" errors="0">
      <testcase classname="fixture" name="passes" />
    </testsuite>
    <testsuite name="second" tests="2" skipped="1" failures="0" errors="0">
      <testcase classname="fixture" name="also_passes" />
      <testcase classname="fixture" name="skips"><skipped /></testcase>
    </testsuite>
  </testsuite>
</testsuites>
"""
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"netft_unit.xunit.xml": xml},
    )

    assert completed.returncode == 0, completed.stdout


@pytest.mark.parametrize(
    ("tests", "skipped", "failures", "errors"),
    [
        ("2", "0", "0", "0"),
        ("1", "1", "0", "0"),
        ("1", "0", "1", "0"),
        ("1", "0", "0", "1"),
    ],
)
def test_summary_rejects_inconsistent_plural_wrapper_counts(
    tmp_path, tests, skipped, failures, errors
):
    xml = """\
<testsuites tests="{tests}" skipped="{skipped}" failures="{failures}" errors="{errors}">
  <testsuite name="passing" tests="1" skipped="0" failures="0" errors="0">
    <testcase classname="fixture" name="passes" />
  </testsuite>
</testsuites>
""".format(
        tests=tests,
        skipped=skipped,
        failures=failures,
        errors=errors,
    )
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"netft_unit.xunit.xml": xml},
    )

    assert completed.returncode != 0


@pytest.mark.parametrize(
    ("tests", "skipped", "failures", "errors"),
    [
        ("2", "0", "0", "0"),
        ("1", "1", "0", "0"),
        ("1", "0", "1", "0"),
        ("1", "0", "0", "1"),
    ],
)
def test_summary_rejects_inconsistent_root_suite_counts(
    tmp_path, tests, skipped, failures, errors
):
    xml = """\
<testsuite name="root" tests="{tests}" skipped="{skipped}" failures="{failures}" errors="{errors}">
  <testcase classname="fixture" name="passes" />
</testsuite>
""".format(
        tests=tests,
        skipped=skipped,
        failures=failures,
        errors=errors,
    )
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"pytest-netft_unit.xml": xml},
    )

    assert completed.returncode != 0


def test_summary_rejects_an_inconsistent_nested_suite(tmp_path):
    xml = """\
<testsuites tests="1" skipped="0" failures="0" errors="0">
  <testsuite name="parent" tests="2" skipped="0" failures="0" errors="0">
    <testsuite name="leaf" tests="1" skipped="0" failures="0" errors="0">
      <testcase classname="fixture" name="passes" />
    </testsuite>
  </testsuite>
</testsuites>
"""
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"netft_unit.xml": xml},
    )

    assert completed.returncode != 0


@pytest.mark.parametrize("attribute", ["tests", "skipped", "failures", "errors"])
def test_summary_rejects_negative_counts(tmp_path, attribute):
    xml = """\
<testsuite name="negative" {attribute}="-1">
  <testcase classname="fixture" name="passes" />
</testsuite>
""".format(attribute=attribute)
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"pytest-netft_unit.xml": xml},
    )

    assert completed.returncode != 0


@pytest.mark.parametrize("attribute", ["tests", "skipped", "failures", "errors"])
def test_summary_rejects_non_integer_counts(tmp_path, attribute):
    xml = """\
<testsuites {attribute}="not-a-count">
  <testsuite name="passing" tests="1" skipped="0" failures="0" errors="0">
    <testcase classname="fixture" name="passes" />
  </testsuite>
</testsuites>
""".format(attribute=attribute)
    completed = _run_validator(
        tmp_path,
        "netft_unit",
        "at-least-one",
        {"netft_unit.xunit.xml": xml},
    )

    assert completed.returncode != 0
