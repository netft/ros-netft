import rclpy
from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue
from geometry_msgs.msg import WrenchStamped
from rclpy._rclpy_pybind11 import RCLError
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from std_srvs.srv import Trigger

from .client import NetFTClient, NotConnectedError
from .node_common import (
    DEFAULT_PARAMETERS,
    DiagnosticEvaluator,
    FaultLogThrottle,
    build_diagnostic_values,
    log_diagnostic_fault,
    node_config_from_mapping,
)


class NetFTROS2Node(Node):
    def __init__(self):
        super().__init__("netft")
        for name, default in DEFAULT_PARAMETERS.items():
            self.declare_parameter(name, default)
        values = {
            name: self.get_parameter(name).value for name in DEFAULT_PARAMETERS
        }
        self.config = node_config_from_mapping(values)
        self.client = NetFTClient(self.config.client)
        self._diagnostics = DiagnosticEvaluator(
            self.config.expected_rdt_rate, self.config.rate_tolerance
        )
        self._fault_logs = FaultLogThrottle()
        self._wrench_publisher = self.create_publisher(
            WrenchStamped, self.config.wrench_topic, qos_profile_sensor_data
        )
        self._diagnostic_publisher = self.create_publisher(
            DiagnosticArray, "/diagnostics", 10
        )
        self._bias_server = self.create_service(
            Trigger, self.config.bias_service, self._handle_bias
        )
        self._diagnostic_timer = self.create_timer(
            1.0 / self.config.diagnostics_rate, self._publish_diagnostics
        )
        self.client.start(self._publish_wrench)

    def stop(self):
        self.client.stop()

    def _publish_wrench(self, sample):
        message = WrenchStamped()
        message.header.stamp = self.get_clock().now().to_msg()
        message.header.frame_id = self.config.frame_id
        message.wrench.force.x, message.wrench.force.y, message.wrench.force.z = (
            sample.force
        )
        message.wrench.torque.x, message.wrench.torque.y, message.wrench.torque.z = (
            sample.torque
        )
        self._wrench_publisher.publish(message)

    def _handle_bias(self, request, response):
        try:
            self.client.bias()
        except (NotConnectedError, OSError) as exc:
            response.success = False
            response.message = str(exc)
            return response
        response.success = True
        response.message = "software bias command sent and RDT streaming restarted"
        return response

    def _publish_diagnostics(self):
        snapshot = self.client.health_snapshot()
        report = self._diagnostics.evaluate(snapshot)
        logger = self.get_logger()
        log_diagnostic_fault(
            report,
            self._fault_logs,
            logger.warning,
            logger.error,
        )
        status = DiagnosticStatus()
        status.level = bytes((report.level,))
        status.name = "netft_driver: connection"
        status.message = report.message
        status.hardware_id = "{}:{}".format(
            snapshot.sensor_host, snapshot.sensor_port
        )
        status.values = build_diagnostic_values(report, KeyValue)
        array = DiagnosticArray()
        array.header.stamp = self.get_clock().now().to_msg()
        array.status = [status]
        self._diagnostic_publisher.publish(array)


def main(args=None):
    rclpy.init(args=args)
    node = None
    try:
        node = NetFTROS2Node()
        rclpy.spin(node)
    except ExternalShutdownException:
        pass
    except RCLError:
        if rclpy.ok():
            raise
    except (TypeError, ValueError) as exc:
        if node is not None:
            node.get_logger().fatal("invalid Net F/T configuration: {}".format(exc))
        else:
            print("invalid Net F/T configuration: {}".format(exc))
        return 2
    finally:
        if node is not None:
            node.stop()
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return 0
