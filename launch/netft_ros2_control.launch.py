from xml.dom import minidom

import xacro
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def _nodes(context):
    share = get_package_share_directory("netft_driver")
    controller_config = f"{share}/config/netft_ros2_control.yaml"
    values = {
        name: LaunchConfiguration(name).perform(context)
        for name in (
            "sensor_name",
            "sensor_ip",
            "sensor_port",
            "http_port",
            "use_sensor_calibration",
            "counts_per_force",
            "counts_per_torque",
            "receive_timeout",
            "configuration_connect_timeout",
            "configuration_timeout",
            "activation_timeout",
        )
    }
    wrapper = f"""<?xml version='1.0'?>
<robot name='netft' xmlns:xacro='http://www.ros.org/wiki/xacro'>
  <link name='base'/>
  <xacro:include filename='{share}/urdf/netft.ros2_control.xacro'/>
  <xacro:netft_ros2_control name='netft_hardware'
    sensor_name='{values['sensor_name']}' sensor_ip='{values['sensor_ip']}'
    sensor_port='{values['sensor_port']}' http_port='{values['http_port']}'
    use_sensor_calibration='{values['use_sensor_calibration']}'
    counts_per_force='{values['counts_per_force']}'
    counts_per_torque='{values['counts_per_torque']}'
    receive_timeout='{values['receive_timeout']}'
    configuration_connect_timeout='{values['configuration_connect_timeout']}'
    configuration_timeout='{values['configuration_timeout']}'
    activation_timeout='{values['activation_timeout']}'/>
</robot>"""
    document = minidom.parseString(wrapper)
    xacro.process_doc(document)
    return [
        Node(
            package="robot_state_publisher",
            executable="robot_state_publisher",
            parameters=[{"robot_description": document.toxml()}],
            output="screen",
        ),
        Node(
            package="controller_manager",
            executable="ros2_control_node",
            parameters=[controller_config],
            remappings=[("~/robot_description", "/robot_description")],
            output="screen",
        ),
        Node(
            package="controller_manager",
            executable="spawner",
            arguments=[
                "netft_broadcaster",
                "--controller-manager",
                "/controller_manager",
                "--param-file",
                controller_config,
            ],
            output="screen",
        ),
    ]


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument("sensor_name", default_value="netft_sensor"),
            DeclareLaunchArgument("sensor_ip", default_value="192.168.1.1"),
            DeclareLaunchArgument("sensor_port", default_value="49152"),
            DeclareLaunchArgument("http_port", default_value="80"),
            DeclareLaunchArgument("use_sensor_calibration", default_value="true"),
            DeclareLaunchArgument("counts_per_force", default_value="1000000"),
            DeclareLaunchArgument("counts_per_torque", default_value="1000000"),
            DeclareLaunchArgument("receive_timeout", default_value="0.1"),
            DeclareLaunchArgument(
                "configuration_connect_timeout", default_value="0.5"
            ),
            DeclareLaunchArgument("configuration_timeout", default_value="1.0"),
            DeclareLaunchArgument("activation_timeout", default_value="2.0"),
            OpaqueFunction(function=_nodes),
        ]
    )
