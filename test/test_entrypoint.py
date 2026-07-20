import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_node_dispatcher_rejects_unknown_ros_version_without_importing_ros():
    environment = dict(os.environ)
    environment["ROS_VERSION"] = "3"
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "netft_node")],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "ROS_VERSION must be 1 or 2" in result.stderr
