#!/usr/bin/env python3
"""Validate executed test counts in one native package JUnit result."""

import sys
from pathlib import Path
from xml.etree import ElementTree


EXPECTATIONS = {"at-least-one", "exactly-one"}
COUNT_ATTRIBUTES = ("tests", "skipped", "failures", "errors")


def _local_name(tag):
    return tag.rsplit("}", 1)[-1]


def _discover_result(results_root, target):
    accepted_names = {
        "{}.xml".format(target),
        "{}.xunit.xml".format(target),
        "{}.gtest.xml".format(target),
        "gtest-{}.xml".format(target),
        "pytest-{}.xml".format(target),
    }
    matches = sorted(
        path
        for path in results_root.rglob("*.xml")
        if path.name in accepted_names
    )
    if not matches:
        raise ValueError(
            "{}: found no JUnit XML below {}".format(target, results_root)
        )
    if len(matches) != 1:
        raise ValueError(
            "{}: found multiple JUnit XML files below {}: {}".format(
                target,
                results_root,
                ", ".join(str(path) for path in matches),
            )
        )
    return matches[0]


def _case_counts(cases):
    counts = {attribute: 0 for attribute in COUNT_ATTRIBUTES}
    counts["tests"] = len(cases)
    for case in cases:
        outcomes = {_local_name(child.tag) for child in case}
        counts["skipped"] += "skipped" in outcomes
        counts["failures"] += "failure" in outcomes
        counts["errors"] += "error" in outcomes
    return counts


def _summary_label(element):
    kind = _local_name(element.tag)
    name = element.attrib.get("name")
    if kind == "testsuite" and name:
        return "testsuite {!r}".format(name)
    return kind


def _declared_count(element, attribute):
    value = element.attrib[attribute]
    try:
        count = int(value)
    except ValueError:
        count = -1
    if count < 0:
        raise ValueError(
            "{} attribute {!r} must be a non-negative integer; got {!r}".format(
                _local_name(element.tag), attribute, value
            )
        )
    return count


def _validate_summaries(elements):
    summaries = [
        element
        for element in elements
        if _local_name(element.tag) in {"testsuite", "testsuites"}
    ]
    for summary in summaries:
        cases = [
            element
            for element in summary.iter()
            if _local_name(element.tag) == "testcase"
        ]
        actual = _case_counts(cases)
        for attribute in COUNT_ATTRIBUTES:
            if attribute not in summary.attrib:
                continue
            declared = _declared_count(summary, attribute)
            if declared != actual[attribute]:
                raise ValueError(
                    "{} declares {} {} but contains {} descendant testcase outcome(s)".format(
                        _summary_label(summary),
                        declared,
                        attribute,
                        actual[attribute],
                    )
                )


def _result_counts(path):
    root = ElementTree.parse(str(path)).getroot()
    elements = list(root.iter())
    cases = [item for item in elements if _local_name(item.tag) == "testcase"]
    _validate_summaries(elements)
    counts = _case_counts(cases)
    executed = counts["tests"] - counts["skipped"]
    return executed, counts["skipped"], counts["failures"], counts["errors"]


def validate(results_root, target, expectation):
    if expectation not in EXPECTATIONS:
        raise ValueError(
            "expectation must be one of: {}".format(
                ", ".join(sorted(EXPECTATIONS))
            )
        )
    path = _discover_result(results_root, target)
    executed, skipped, failures, errors = _result_counts(path)

    if failures or errors:
        raise ValueError(
            "{}: {} failure(s), {} error(s) in {}".format(
                target, failures, errors, path
            )
        )
    if expectation == "at-least-one" and executed < 1:
        raise ValueError(
            "{}: requires at least one non-skipped executed test; found {} in {}".format(
                target, executed, path
            )
        )
    if expectation == "exactly-one" and executed != 1:
        raise ValueError(
            "{}: requires exactly one non-skipped executed test; found {} in {}".format(
                target, executed, path
            )
        )

    print(
        "{}: {} non-skipped executed, {} skipped, 0 failures, 0 errors; XML {}".format(
            target, executed, skipped, path
        )
    )


def main(argv):
    if len(argv) != 4:
        print(
            "usage: validate_junit.py RESULTS_ROOT TARGET "
            "{at-least-one,exactly-one}",
            file=sys.stderr,
        )
        return 2
    try:
        validate(Path(argv[1]), argv[2], argv[3])
    except (ElementTree.ParseError, OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
