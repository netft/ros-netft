import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
HARNESSES = {
    "ros1": ROOT / "test/integration/ros1_smoke_harness_test.sh",
    "ros2": ROOT / "test/integration/ros2_smoke_harness_test.sh",
}


def test_selected_shell_harness():
    selection = os.environ.get("NETFT_HARNESS")
    if selection is None:
        pytest.skip("NETFT_HARNESS is set only by the native package test target")
    assert selection in HARNESSES, selection
    completed = subprocess.run(
        ["bash", str(HARNESSES[selection])],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stdout
