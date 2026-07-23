from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
XACRO_NAMESPACE = "http://www.ros.org/wiki/xacro"
EXPECTED_DEFAULTS = {
    "sensor_port": "49152",
    "http_port": "80",
    "use_sensor_calibration": "true",
    "configuration_connect_timeout": "0.5",
    "configuration_timeout": "1.0",
}


def xacro_parameters():
    root = ElementTree.parse(ROOT / "urdf/netft.ros2_control.xacro").getroot()
    macro = root.find(f"{{{XACRO_NAMESPACE}}}macro")
    assert macro is not None
    return {
        token.partition(":=")[0]: token.partition(":=")[2]
        for token in macro.attrib["params"].split()
    }


def test_ros2_control_xacro_exposes_automatic_sensor_calibration_defaults():
    parameters = xacro_parameters()
    assert parameters["sensor_ip"] == ""
    assert {name: parameters[name] for name in EXPECTED_DEFAULTS} == EXPECTED_DEFAULTS
    assert parameters["counts_per_force"] == "1000000"
    assert parameters["counts_per_torque"] == "1000000"


def test_ros2_control_xacro_forwards_connection_parameters_to_hardware_info():
    root = ElementTree.parse(ROOT / "urdf/netft.ros2_control.xacro").getroot()
    parameters = {
        item.attrib["name"]: item.text
        for item in root.findall(".//hardware/param")
    }
    expected_names = {
        "sensor_ip",
        "sensor_port",
        "http_port",
        "use_sensor_calibration",
        "configuration_connect_timeout",
        "configuration_timeout",
    }
    assert {name: parameters[name] for name in expected_names} == {
        name: "${" + name + "}" for name in expected_names
    }
