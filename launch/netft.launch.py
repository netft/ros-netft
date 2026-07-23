from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from ament_index_python.packages import get_package_share_directory
import os


def generate_launch_description():
    config = os.path.join(
        get_package_share_directory("netft_driver"), "config", "netft_ros2.yaml"
    )
    return LaunchDescription(
        [
            DeclareLaunchArgument("sensor_ip", default_value="192.168.1.1"),
            DeclareLaunchArgument("sensor_port", default_value="49152"),
            DeclareLaunchArgument("http_port", default_value="80"),
            DeclareLaunchArgument("use_sensor_calibration", default_value="true"),
            DeclareLaunchArgument(
                "configuration_connect_timeout", default_value="0.5"
            ),
            DeclareLaunchArgument("configuration_timeout", default_value="1.0"),
            Node(
                package="netft_driver",
                executable="netft_node",
                name="netft",
                output="screen",
                parameters=[
                    config,
                    {
                        "sensor_ip": LaunchConfiguration("sensor_ip"),
                        "sensor_port": ParameterValue(
                            LaunchConfiguration("sensor_port"), value_type=int
                        ),
                        "http_port": ParameterValue(
                            LaunchConfiguration("http_port"), value_type=int
                        ),
                        "use_sensor_calibration": ParameterValue(
                            LaunchConfiguration("use_sensor_calibration"),
                            value_type=bool,
                        ),
                        "configuration_connect_timeout": ParameterValue(
                            LaunchConfiguration("configuration_connect_timeout"),
                            value_type=float,
                        ),
                        "configuration_timeout": ParameterValue(
                            LaunchConfiguration("configuration_timeout"),
                            value_type=float,
                        ),
                    },
                ],
            ),
        ]
    )
