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
        for name in ("sensor_name", "sensor_ip", "sensor_port")
    }
    wrapper = f"""<?xml version='1.0'?>
<robot name='netft' xmlns:xacro='http://www.ros.org/wiki/xacro'>
  <link name='base'/>
  <xacro:include filename='{share}/urdf/netft.ros2_control.xacro'/>
  <xacro:netft_ros2_control name='netft_hardware'
    sensor_name='{values['sensor_name']}' sensor_ip='{values['sensor_ip']}'
    sensor_port='{values['sensor_port']}'/>
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
            OpaqueFunction(function=_nodes),
        ]
    )
