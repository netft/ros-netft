from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]


def test_manifest_has_conditional_ros_build_types_and_dependencies():
    root = ElementTree.parse(str(ROOT / "package.xml")).getroot()
    assert root.attrib["format"] == "3"
    assert root.findtext("name") == "netft_driver"
    dependencies = {
        (element.tag, element.text.strip(), element.attrib.get("condition"))
        for element in root
        if element.tag.endswith("depend")
    }
    assert ("buildtool_depend", "catkin", "$ROS_VERSION == 1") in dependencies
    assert ("buildtool_depend", "ament_cmake", "$ROS_VERSION == 2") in dependencies
    assert ("build_depend", "ament_cmake_python", "$ROS_VERSION == 2") not in dependencies
    assert ("exec_depend", "rospy", "$ROS_VERSION == 1") not in dependencies
    assert ("exec_depend", "rclpy", "$ROS_VERSION == 2") not in dependencies
    assert ("test_depend", "rclpy", "$ROS_VERSION == 2") in dependencies
    build_types = {
        (item.text.strip(), item.attrib.get("condition"))
        for item in root.findall("./export/build_type")
    }
    assert build_types == {
        ("catkin", "$ROS_VERSION == 1"),
        ("ament_cmake", "$ROS_VERSION == 2"),
    }
