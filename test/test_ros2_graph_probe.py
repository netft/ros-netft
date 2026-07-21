from types import SimpleNamespace
import inspect

import pytest
import test.integration.ros2_graph_probe as graph_probe

from test.integration.ros2_graph_probe import (
    Deadline,
    GraphSnapshot,
    diagnostics_to_yaml,
    graph_issues,
    wait_until,
    wrench_differs,
    wrench_values,
    wrench_to_yaml,
    ProbeRuntime,
    parse_arguments,
)


def test_wait_until_uses_one_shared_deadline_and_reports_state():
    clock = [10.0]
    deadline = Deadline.after(1.0, now=lambda: clock[0])

    def spin_once(duration):
        clock[0] += duration

    with pytest.raises(TimeoutError, match=r"graph.*nodes=\[\]"):
        wait_until(
            "graph discovery",
            lambda: False,
            spin_once,
            deadline,
            lambda: "nodes=[]",
            now=lambda: clock[0],
        )

    assert clock[0] == pytest.approx(11.0)


def test_graph_contract_ties_interfaces_to_the_expected_node():
    healthy = GraphSnapshot(
        nodes={"/netft_smoke/netft"},
        publishers={
            "/netft_smoke/remapped_wrench": ["geometry_msgs/msg/WrenchStamped"]
        },
        services={"/netft_smoke/bias": ["std_srvs/srv/Trigger"]},
        topics={
            "/netft_smoke/remapped_wrench": ["geometry_msgs/msg/WrenchStamped"]
        },
    )
    assert graph_issues(
        healthy,
        node_name="/netft_smoke/netft",
        service_name="/netft_smoke/bias",
        wrench_topic="/netft_smoke/remapped_wrench",
        absent_topic="/netft_smoke/wrench",
    ) == []

    wrong_owner = GraphSnapshot(
        nodes=healthy.nodes,
        publishers={},
        services=healthy.services,
        topics=healthy.topics,
    )
    assert "expected node does not publish the wrench topic" in graph_issues(
        wrong_owner,
        node_name="/netft_smoke/netft",
        service_name="/netft_smoke/bias",
        wrench_topic="/netft_smoke/remapped_wrench",
        absent_topic="/netft_smoke/wrench",
    )


def test_probe_yaml_is_consumable_by_the_shared_assertion_boundary():
    stamp = SimpleNamespace(sec=17, nanosec=250_000_000)
    wrench = SimpleNamespace(
        header=SimpleNamespace(stamp=stamp, frame_id="smoke_frame"),
        wrench=SimpleNamespace(
            force=SimpleNamespace(x=0.1, y=-0.2, z=0.3),
            torque=SimpleNamespace(x=0.01, y=-0.02, z=0.03),
        ),
    )
    wrench_yaml = wrench_to_yaml(wrench)
    assert wrench_values(wrench) == (0.1, -0.2, 0.3, 0.01, -0.02, 0.03)
    assert not wrench_differs(wrench, (0.1 + 1e-13, -0.2, 0.3, 0.01, -0.02, 0.03))
    assert wrench_differs(wrench, (0.2, -0.2, 0.3, 0.01, -0.02, 0.03))
    assert 'frame_id: "smoke_frame"' in wrench_yaml
    assert "x: 0.1" in wrench_yaml
    assert "z: 0.03" in wrench_yaml

    status = SimpleNamespace(
        level=0,
        name="netft_driver: connection",
        message="streaming",
        hardware_id="127.0.0.1:49152",
        values=[SimpleNamespace(key="state", value="streaming")],
    )
    diagnostic = SimpleNamespace(
        header=SimpleNamespace(stamp=stamp, frame_id=""), status=[status]
    )
    diagnostics_yaml = diagnostics_to_yaml([diagnostic, diagnostic])
    assert diagnostics_yaml.count("netft_driver: connection") == 2
    assert 'hardware_id: "127.0.0.1:49152"' in diagnostics_yaml
    assert "---" in diagnostics_yaml


def test_subscription_callbacks_are_python_methods_with_stable_signatures():
    assert list(inspect.signature(ProbeRuntime._on_wrench).parameters) == [
        "self",
        "message",
    ]
    assert list(inspect.signature(ProbeRuntime._on_diagnostics).parameters) == [
        "self",
        "message",
    ]


def test_control_probe_accepts_multiple_services_and_an_explicit_bias_target():
    arguments = parse_arguments(
        [
            "control",
            "--service-name",
            "/left_ft/bias",
            "--service-name",
            "/right_ft/bias",
            "--wrench-topic",
            "/left_broadcaster/wrench",
            "--wrench-topic",
            "/right_broadcaster/wrench",
            "--bias-service",
            "/right_ft/bias",
            "--bias-wrench-topic",
            "/right_broadcaster/wrench",
            "--timeout",
            "12",
        ]
    )

    assert arguments.mode == "control"
    assert arguments.service_names == ["/left_ft/bias", "/right_ft/bias"]
    assert arguments.wrench_topics == [
        "/left_broadcaster/wrench",
        "/right_broadcaster/wrench",
    ]
    assert arguments.bias_service == "/right_ft/bias"
    assert arguments.bias_wrench_topic == "/right_broadcaster/wrench"
    assert arguments.timeout == 12.0


def test_post_bias_wait_ignores_wrenches_received_before_the_service_response():
    class Future:
        completed = False

        def done(self):
            return self.completed

        def result(self):
            return SimpleNamespace(success=True, message="bias sent")

    class Client:
        def __init__(self, future):
            self.future = future

        def call_async(self, request):
            assert request is not None
            return self.future

    class Runtime:
        def __init__(self, future):
            self.future = future
            self.wrench_counts = {"/right_broadcaster/wrench": 1}
            self.spin_count = 0

        def spin_once(self, duration):
            assert duration > 0.0
            self.spin_count += 1
            self.wrench_counts["/right_broadcaster/wrench"] += 1
            if self.spin_count == 1:
                self.future.completed = True

        def state(self):
            return f"wrench_messages={self.wrench_counts}"

    future = Future()
    runtime = Runtime(future)
    graph_probe.call_bias_and_wait_for_post_bias_wrench(
        runtime,
        Client(future),
        "/right_broadcaster/wrench",
        Deadline.after(1.0),
        lambda: object(),
    )

    assert runtime.spin_count == 2
    assert runtime.wrench_counts["/right_broadcaster/wrench"] == 3
