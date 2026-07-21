#!/usr/bin/env python3
import argparse
from collections import deque
from dataclasses import dataclass
import json
import os
from pathlib import Path
import sys
import time


WRENCH_TYPE = "geometry_msgs/msg/WrenchStamped"
TRIGGER_TYPE = "std_srvs/srv/Trigger"


@dataclass(frozen=True)
class Deadline:
    expires_at: float

    @classmethod
    def after(cls, timeout, *, now=time.monotonic):
        if timeout <= 0.0:
            raise ValueError("timeout must be greater than zero")
        return cls(now() + timeout)

    def remaining(self, *, now=time.monotonic):
        return max(0.0, self.expires_at - now())


@dataclass
class GraphSnapshot:
    nodes: set
    publishers: dict
    services: dict
    topics: dict


def wait_until(label, predicate, spin_once, deadline, describe, *, now=time.monotonic):
    while True:
        if predicate():
            return
        remaining = deadline.remaining(now=now)
        if remaining <= 0.0:
            raise TimeoutError(f"timed out waiting for {label}: {describe()}")
        spin_once(min(0.1, remaining))


def graph_issues(
    snapshot,
    *,
    node_name,
    service_name,
    wrench_topic,
    absent_topic=None,
):
    issues = []
    if node_name not in snapshot.nodes:
        issues.append("expected node is not visible")
    if snapshot.publishers.get(wrench_topic) != [WRENCH_TYPE]:
        issues.append("expected node does not publish the wrench topic")
    if snapshot.services.get(service_name) != [TRIGGER_TYPE]:
        issues.append("expected node does not provide the bias service")
    if snapshot.topics.get(wrench_topic) != [WRENCH_TYPE]:
        issues.append("wrench topic type is missing or incorrect")
    if absent_topic and absent_topic in snapshot.topics:
        issues.append("unremapped wrench topic unexpectedly exists")
    return issues


def _yaml_string(value):
    return json.dumps(str(value), ensure_ascii=True)


def _stamp_lines(stamp, indent):
    return [
        f"{indent}sec: {stamp.sec}",
        f"{indent}nanosec: {stamp.nanosec}",
    ]


def wrench_to_yaml(message):
    lines = ["header:", "  stamp:"]
    lines.extend(_stamp_lines(message.header.stamp, "    "))
    lines.extend(
        [
            f"  frame_id: {_yaml_string(message.header.frame_id)}",
            "wrench:",
            "  force:",
            f"    x: {message.wrench.force.x!r}",
            f"    y: {message.wrench.force.y!r}",
            f"    z: {message.wrench.force.z!r}",
            "  torque:",
            f"    x: {message.wrench.torque.x!r}",
            f"    y: {message.wrench.torque.y!r}",
            f"    z: {message.wrench.torque.z!r}",
        ]
    )
    return "\n".join(lines) + "\n"


def wrench_values(message):
    return (
        message.wrench.force.x,
        message.wrench.force.y,
        message.wrench.force.z,
        message.wrench.torque.x,
        message.wrench.torque.y,
        message.wrench.torque.z,
    )


def wrench_differs(message, baseline, *, tolerance=1e-12):
    return any(
        abs(observed - expected) > tolerance
        for observed, expected in zip(wrench_values(message), baseline)
    )


def _diagnostic_to_yaml(message):
    lines = ["header:", "  stamp:"]
    lines.extend(_stamp_lines(message.header.stamp, "    "))
    lines.extend(
        [
            f"  frame_id: {_yaml_string(message.header.frame_id)}",
            "status:",
        ]
    )
    for status in message.status:
        lines.extend(
            [
                f"- level: {status.level}",
                f"  name: {_yaml_string(status.name)}",
                f"  message: {_yaml_string(status.message)}",
                f"  hardware_id: {_yaml_string(status.hardware_id)}",
                "  values:",
            ]
        )
        for value in status.values:
            lines.extend(
                [
                    f"  - key: {_yaml_string(value.key)}",
                    f"    value: {_yaml_string(value.value)}",
                ]
            )
    return "\n".join(lines)


def diagnostics_to_yaml(messages):
    return "\n---\n".join(_diagnostic_to_yaml(message) for message in messages) + "\n"


def _canonical_node_name(name, namespace):
    if namespace == "/":
        return "/" + name
    return namespace.rstrip("/") + "/" + name


def _split_node_name(fully_qualified_name):
    parts = [part for part in fully_qualified_name.split("/") if part]
    if not parts:
        raise ValueError("node name must be fully qualified and non-empty")
    namespace = "/" + "/".join(parts[:-1]) if len(parts) > 1 else "/"
    return parts[-1], namespace


class ProbeRuntime:
    def __init__(self, arguments):
        import rclpy
        from diagnostic_msgs.msg import DiagnosticArray
        from geometry_msgs.msg import WrenchStamped
        from rclpy.qos import qos_profile_sensor_data
        from std_srvs.srv import Trigger

        self.rclpy = rclpy
        self.target_node_name, self.target_namespace = _split_node_name(
            arguments.node_name
        )
        rclpy.init(args=[])
        self.node = rclpy.create_node(f"netft_graph_probe_{os.getpid()}")
        self.wrench_messages = deque(maxlen=4)
        self.wrench_count = 0
        self.diagnostics = deque(maxlen=100)
        self.last_snapshot = GraphSnapshot(set(), {}, {}, {})
        self.wrench_subscription = self.node.create_subscription(
            WrenchStamped,
            arguments.wrench_topic,
            self._on_wrench,
            qos_profile_sensor_data,
        )
        self.diagnostics_subscription = None
        if getattr(arguments, "diagnostics_topic", None):
            self.diagnostics_subscription = self.node.create_subscription(
                DiagnosticArray,
                arguments.diagnostics_topic,
                self._on_diagnostics,
                100,
            )
        self.bias_client = self.node.create_client(Trigger, arguments.service_name)

    def _on_wrench(self, message):
        self.wrench_count += 1
        self.wrench_messages.append(message)

    def _on_diagnostics(self, message):
        self.diagnostics.append(message)

    def spin_once(self, duration):
        self.rclpy.spin_once(self.node, timeout_sec=duration)

    def snapshot(self):
        nodes = {
            _canonical_node_name(name, namespace)
            for name, namespace in self.node.get_node_names_and_namespaces()
        }
        publishers = {}
        services = {}
        if _canonical_node_name(self.target_node_name, self.target_namespace) in nodes:
            publishers = dict(
                self.node.get_publisher_names_and_types_by_node(
                    self.target_node_name, self.target_namespace
                )
            )
            services = dict(
                self.node.get_service_names_and_types_by_node(
                    self.target_node_name, self.target_namespace
                )
            )
        self.last_snapshot = GraphSnapshot(
            nodes=nodes,
            publishers=publishers,
            services=services,
            topics=dict(self.node.get_topic_names_and_types()),
        )
        return self.last_snapshot

    def state(self):
        snapshot = self.last_snapshot
        last_wrench_axes = (
            wrench_values(self.wrench_messages[-1]) if self.wrench_messages else None
        )
        return (
            f"nodes={sorted(snapshot.nodes)}, "
            f"target_publishers={snapshot.publishers}, "
            f"target_services={snapshot.services}, "
            f"topics={snapshot.topics}, "
            f"client_ready={self.bias_client.service_is_ready()}, "
            f"wrench_messages={self.wrench_count}, "
            f"last_wrench_axes={last_wrench_axes}, "
            f"diagnostics_messages={len(self.diagnostics)}"
        )

    def close(self):
        self.node.destroy_node()
        if self.rclpy.ok():
            self.rclpy.shutdown()


class ControlProbeRuntime:
    def __init__(self, arguments):
        import rclpy
        from geometry_msgs.msg import WrenchStamped
        from rclpy.qos import qos_profile_sensor_data
        from std_srvs.srv import Trigger

        self.rclpy = rclpy
        self.service_names = tuple(arguments.service_names)
        self.wrench_topics = tuple(arguments.wrench_topics)
        rclpy.init(args=[])
        self.node = rclpy.create_node(f"netft_control_probe_{os.getpid()}")
        self.wrench_counts = {topic: 0 for topic in self.wrench_topics}
        self.last_snapshot = GraphSnapshot(set(), {}, {}, {})
        self.wrench_subscriptions = [
            self.node.create_subscription(
                WrenchStamped,
                topic,
                lambda message, topic=topic: self._on_wrench(topic, message),
                qos_profile_sensor_data,
            )
            for topic in self.wrench_topics
        ]
        self.bias_clients = {
            service_name: self.node.create_client(Trigger, service_name)
            for service_name in self.service_names
        }

    def _on_wrench(self, topic, message):
        del message
        self.wrench_counts[topic] += 1

    def spin_once(self, duration):
        self.rclpy.spin_once(self.node, timeout_sec=duration)

    def snapshot(self):
        self.last_snapshot = GraphSnapshot(
            nodes={
                _canonical_node_name(name, namespace)
                for name, namespace in self.node.get_node_names_and_namespaces()
            },
            publishers={},
            services=dict(self.node.get_service_names_and_types()),
            topics=dict(self.node.get_topic_names_and_types()),
        )
        return self.last_snapshot

    def state(self):
        snapshot = self.last_snapshot
        return (
            f"nodes={sorted(snapshot.nodes)}, "
            f"services={snapshot.services}, "
            f"topics={snapshot.topics}, "
            f"client_ready={{{', '.join(f'{name!r}: {client.service_is_ready()}' for name, client in self.bias_clients.items())}}}, "
            f"wrench_messages={self.wrench_counts}"
        )

    def close(self):
        self.node.destroy_node()
        if self.rclpy.ok():
            self.rclpy.shutdown()


def _wait_for_graph(runtime, arguments, deadline):
    def ready():
        snapshot = runtime.snapshot()
        return not graph_issues(
            snapshot,
            node_name=arguments.node_name,
            service_name=arguments.service_name,
            wrench_topic=arguments.wrench_topic,
            absent_topic=arguments.absent_topic,
        )

    wait_until(
        "expected node, service, and wrench graph",
        ready,
        runtime.spin_once,
        deadline,
        runtime.state,
    )
    wait_until(
        "bias client readiness",
        runtime.bias_client.service_is_ready,
        runtime.spin_once,
        deadline,
        runtime.state,
    )


def _write(path, content):
    Path(path).write_text(content, encoding="utf-8")


def run_ready(arguments):
    deadline = Deadline.after(arguments.timeout)
    runtime = ProbeRuntime(arguments)
    try:
        _wait_for_graph(runtime, arguments, deadline)
        wait_until(
            "first wrench message",
            lambda: runtime.wrench_count >= 1,
            runtime.spin_once,
            deadline,
            runtime.state,
        )
        _write(arguments.wrench_output, wrench_to_yaml(runtime.wrench_messages[-1]))
    finally:
        runtime.close()


def run_full(arguments):
    from std_srvs.srv import Trigger

    deadline = Deadline.after(arguments.timeout)
    runtime = ProbeRuntime(arguments)
    try:
        _wait_for_graph(runtime, arguments, deadline)
        wait_until(
            "pre-bias wrench and five diagnostics messages",
            lambda: runtime.wrench_count >= 1 and len(runtime.diagnostics) >= 5,
            runtime.spin_once,
            deadline,
            runtime.state,
        )
        pre_bias_wrench = runtime.wrench_messages[-1]
        pre_bias_values = wrench_values(pre_bias_wrench)
        diagnostics = list(runtime.diagnostics)

        future = runtime.bias_client.call_async(Trigger.Request())
        wait_until(
            "software-bias service response",
            future.done,
            runtime.spin_once,
            deadline,
            runtime.state,
        )
        response = future.result()
        if response is None:
            raise RuntimeError("software-bias service completed without a response")
        if not response.success:
            raise RuntimeError(f"software-bias service failed: {response.message}")

        post_bias_baseline = runtime.wrench_count
        wait_until(
            "post-bias wrench values different from pre-bias values",
            lambda: runtime.wrench_count > post_bias_baseline
            and wrench_differs(runtime.wrench_messages[-1], pre_bias_values),
            runtime.spin_once,
            deadline,
            runtime.state,
        )
        _write(arguments.wrench_output, wrench_to_yaml(pre_bias_wrench))
        _write(arguments.post_bias_output, wrench_to_yaml(runtime.wrench_messages[-1]))
        _write(arguments.diagnostics_output, diagnostics_to_yaml(diagnostics))
        _write(
            arguments.bias_output,
            f"success=True\nmessage={response.message}\n",
        )
    finally:
        runtime.close()


def call_bias_and_wait_for_post_bias_wrench(
    runtime, bias_client, wrench_topic, deadline, request_factory
):
    future = bias_client.call_async(request_factory())
    wait_until(
        "software-bias service response",
        future.done,
        runtime.spin_once,
        deadline,
        runtime.state,
    )
    response = future.result()
    if response is None:
        raise RuntimeError("software-bias service completed without a response")
    if not response.success:
        raise RuntimeError(f"software-bias service failed: {response.message}")

    post_bias_baseline = runtime.wrench_counts[wrench_topic]
    wait_until(
        "post-bias broadcaster wrench recovery",
        lambda: runtime.wrench_counts[wrench_topic] > post_bias_baseline,
        runtime.spin_once,
        deadline,
        runtime.state,
    )


def run_control(arguments):
    from std_srvs.srv import Trigger

    deadline = Deadline.after(arguments.timeout)
    runtime = ControlProbeRuntime(arguments)
    try:
        def ready():
            snapshot = runtime.snapshot()
            return (
                all(
                    snapshot.services.get(service_name) == [TRIGGER_TYPE]
                    and runtime.bias_clients[service_name].service_is_ready()
                    for service_name in arguments.service_names
                )
                and all(count >= 1 for count in runtime.wrench_counts.values())
            )

        wait_until(
            "exact bias service type and broadcaster wrench messages",
            ready,
            runtime.spin_once,
            deadline,
            runtime.state,
        )
        if arguments.bias_service is None:
            return
        call_bias_and_wait_for_post_bias_wrench(
            runtime,
            runtime.bias_clients[arguments.bias_service],
            arguments.bias_wrench_topic,
            deadline,
            Trigger.Request,
        )
    finally:
        runtime.close()


def _positive_float(value):
    parsed = float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _add_common_arguments(parser):
    parser.add_argument("--node-name", required=True)
    parser.add_argument("--service-name", required=True)
    parser.add_argument("--wrench-topic", required=True)
    parser.add_argument("--absent-topic")
    parser.add_argument("--wrench-output", required=True, type=Path)
    parser.add_argument("--timeout", type=_positive_float, default=15.0)


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(
        description="Bounded single-participant ROS 2 graph smoke probe"
    )
    subparsers = parser.add_subparsers(dest="mode", required=True)
    ready = subparsers.add_parser("ready")
    _add_common_arguments(ready)
    full = subparsers.add_parser("full")
    _add_common_arguments(full)
    full.add_argument("--diagnostics-topic", required=True)
    full.add_argument("--diagnostics-output", required=True, type=Path)
    full.add_argument("--post-bias-output", required=True, type=Path)
    full.add_argument("--bias-output", required=True, type=Path)
    control = subparsers.add_parser("control")
    control.add_argument(
        "--service-name", dest="service_names", action="append", required=True
    )
    control.add_argument("--wrench-topic", dest="wrench_topics", action="append", required=True)
    control.add_argument("--bias-service")
    control.add_argument("--bias-wrench-topic")
    control.add_argument("--timeout", type=_positive_float, default=15.0)
    arguments = parser.parse_args(argv)
    if arguments.mode == "control":
        if (arguments.bias_service is None) != (arguments.bias_wrench_topic is None):
            parser.error("--bias-service and --bias-wrench-topic must be used together")
        if (
            arguments.bias_service is not None
            and arguments.bias_service not in arguments.service_names
        ):
            parser.error("--bias-service must match one of the --service-name values")
        if (
            arguments.bias_wrench_topic is not None
            and arguments.bias_wrench_topic not in arguments.wrench_topics
        ):
            parser.error("--bias-wrench-topic must match one of the --wrench-topic values")
    return arguments


def main(argv=None):
    arguments = parse_arguments(argv)
    try:
        if arguments.mode == "full":
            run_full(arguments)
        elif arguments.mode == "control":
            run_control(arguments)
        else:
            run_ready(arguments)
    except Exception as error:
        print(f"ROS 2 graph probe failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
