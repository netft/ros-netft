#!/usr/bin/env python3
"""Bounded, loopback-only acceptance of the example DDS bias permissions."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def spin_until(node, predicate, timeout):
    import rclpy

    deadline = time.monotonic() + timeout
    while not predicate():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("secure DDS operation did not complete")
        rclpy.spin_once(node, timeout_sec=min(0.1, remaining))


def probe(role):
    import rclpy
    from geometry_msgs.msg import WrenchStamped
    from rclpy.qos import qos_profile_sensor_data
    from rclpy.impl.implementation_singleton import rclpy_implementation as _rclpy
    from std_srvs.srv import Trigger

    rclpy.init(args=["--ros-args", "--enclave", f"/netft/{role}"])
    node = rclpy.create_node(f"netft_{role}")
    try:
        if role == "reader":
            messages = []
            node.create_subscription(
                WrenchStamped, "/netft/wrench", messages.append, qos_profile_sensor_data
            )
            spin_until(node, lambda: len(messages) >= 3, 8)
            # DDS may reject the forbidden endpoint at creation or matching.
            try:
                client = node.create_client(Trigger, "/netft/bias")
            except _rclpy.RCLError:
                print("reader: secure samples received; bias endpoint rejected")
                return
            if client.wait_for_service(timeout_sec=1):
                future = client.call_async(Trigger.Request())
                deadline = time.monotonic() + 1
                while not future.done() and time.monotonic() < deadline:
                    rclpy.spin_once(node, timeout_sec=0.1)
                if future.done() and future.result() is not None:
                    raise AssertionError("reader was able to call the bias service")
            print("reader: secure samples received; bias request blocked")
        else:
            client = node.create_client(Trigger, "/netft/bias")
            if not client.wait_for_service(timeout_sec=8):
                raise TimeoutError("operator cannot discover the secure bias service")
            future = client.call_async(Trigger.Request())
            spin_until(node, future.done, 3)
            response = future.result()
            if response is None or not response.success:
                raise AssertionError("authorized bias request failed")
            print("operator: authorized bias request succeeded")
    finally:
        node.destroy_node()
        rclpy.shutdown()


def accept(node_executable):
    repo = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repo))
    from test.support.fake_sensor import FakeNetFTSensor

    with tempfile.TemporaryDirectory(prefix="netft-sros2-") as root:
        env = dict(os.environ)
        # Generate permissions for the caller's leased DDS domain. Run in an
        # isolated container/CI host and restrict device I/O to loopback.
        env.update(
            ROS_DOMAIN_ID=os.environ.get("ROS_DOMAIN_ID", "0"),
            ROS_LOCALHOST_ONLY="1",
            RMW_IMPLEMENTATION="rmw_fastrtps_cpp",
        )
        subprocess.run(
            [
                "ros2", "security", "generate_artifacts", "-k", root,
                "-p", str(repo / "docs/bias-policy.example.xml"),
            ], env=env, check=True, timeout=15,
        )
        env.update(
            ROS_SECURITY_ENABLE="true", ROS_SECURITY_STRATEGY="Enforce",
            ROS_SECURITY_KEYSTORE=root,
        )
        with FakeNetFTSensor(rate_hz=200) as sensor:
            with open(Path(root) / "driver.log", "w+") as log:
                driver = subprocess.Popen(
                    [
                        node_executable, "--ros-args", "--enclave", "/netft/driver",
                        "-p", "sensor_ip:=127.0.0.1",
                        "-p", f"sensor_port:={sensor.port}",
                        "-p", f"http_port:={sensor.http_port}",
                        "-p", "wrench_topic:=/netft/wrench",
                        "-p", "bias_service:=/netft/bias",
                        "-p", "receive_timeout:=0.8",
                    ], env=env, stdout=log, stderr=subprocess.STDOUT,
                )
                try:
                    for role in ("reader", "operator"):
                        subprocess.run(
                            [sys.executable, __file__, "--probe", role],
                            env=env, check=True, timeout=15,
                        )
                        bias_count = sum(int(command) == 66 for command in sensor.commands)
                        expected = 0 if role == "reader" else 1
                        if bias_count != expected:
                            raise AssertionError(f"unexpected bias count: {bias_count}")
                    if driver.poll() is not None:
                        raise AssertionError("secure driver exited during acceptance")
                except BaseException:
                    log.seek(0)
                    print(log.read(), file=sys.stderr)
                    raise
                finally:
                    if driver.poll() is None:
                        driver.send_signal(signal.SIGINT)
                    try:
                        driver.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        driver.kill()
                        driver.wait(timeout=2)
                        raise AssertionError("secure driver exceeded shutdown deadline")
                if driver.returncode != 0:
                    raise AssertionError(f"secure driver exit: {driver.returncode}")
            print(json.dumps({"reader_samples": True, "reader_bias": False,
                              "operator_bias": True, "bias_commands": bias_count}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-executable")
    parser.add_argument("--probe", choices=("reader", "operator"))
    args = parser.parse_args()
    if args.probe:
        probe(args.probe)
    elif args.node_executable:
        accept(args.node_executable)
    else:
        parser.error("--node-executable is required")


if __name__ == "__main__":
    main()
