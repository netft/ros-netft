import rospy
from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue
from geometry_msgs.msg import WrenchStamped
from std_srvs.srv import Trigger, TriggerResponse

from .client import NetFTClient, NotConnectedError
from .node_common import (
    DEFAULT_PARAMETERS,
    DiagnosticEvaluator,
    FaultLogThrottle,
    build_diagnostic_values,
    log_diagnostic_fault,
    node_config_from_mapping,
)


class NetFTROS1Node:
    def __init__(self):
        values = {
            name: rospy.get_param("~" + name, default)
            for name, default in DEFAULT_PARAMETERS.items()
        }
        self.config = node_config_from_mapping(values)
        self.client = NetFTClient(self.config.client)
        self._diagnostics = DiagnosticEvaluator(
            self.config.expected_rdt_rate, self.config.rate_tolerance
        )
        self._fault_logs = FaultLogThrottle()
        self._wrench_publisher = rospy.Publisher(
            self.config.wrench_topic, WrenchStamped, queue_size=10
        )
        self._diagnostic_publisher = rospy.Publisher(
            "/diagnostics", DiagnosticArray, queue_size=10
        )
        self._bias_server = rospy.Service(
            self.config.bias_service, Trigger, self._handle_bias
        )
        self._diagnostic_timer = rospy.Timer(
            rospy.Duration(1.0 / self.config.diagnostics_rate),
            self._publish_diagnostics,
        )

    def start(self):
        self.client.start(self._publish_wrench)

    def shutdown(self):
        self.client.stop()

    def _publish_wrench(self, sample):
        message = WrenchStamped()
        message.header.stamp = rospy.Time.now()
        message.header.frame_id = self.config.frame_id
        message.wrench.force.x, message.wrench.force.y, message.wrench.force.z = (
            sample.force
        )
        message.wrench.torque.x, message.wrench.torque.y, message.wrench.torque.z = (
            sample.torque
        )
        self._wrench_publisher.publish(message)

    def _handle_bias(self, request):
        try:
            self.client.bias()
        except (NotConnectedError, OSError) as exc:
            return TriggerResponse(success=False, message=str(exc))
        return TriggerResponse(
            success=True,
            message="software bias command sent and RDT streaming restarted",
        )

    def _publish_diagnostics(self, event):
        snapshot = self.client.health_snapshot()
        report = self._diagnostics.evaluate(snapshot)
        log_diagnostic_fault(
            report,
            self._fault_logs,
            rospy.logwarn,
            rospy.logerr,
        )
        status = DiagnosticStatus()
        status.level = report.level
        status.name = "netft_driver: connection"
        status.message = report.message
        status.hardware_id = "{}:{}".format(
            snapshot.sensor_host, snapshot.sensor_port
        )
        status.values = build_diagnostic_values(report, KeyValue)
        array = DiagnosticArray()
        array.header.stamp = rospy.Time.now()
        array.status = [status]
        self._diagnostic_publisher.publish(array)


def main():
    rospy.init_node("netft")
    try:
        node = NetFTROS1Node()
    except (TypeError, ValueError) as exc:
        rospy.logfatal("invalid Net F/T configuration: %s", exc)
        return 2
    rospy.on_shutdown(node.shutdown)
    node.start()
    rospy.spin()
    node.shutdown()
    return 0
