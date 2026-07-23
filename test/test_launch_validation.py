import ast
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_ARGUMENT_DEFAULTS = {
    "sensor_ip": "192.168.1.1",
    "sensor_port": "49152",
    "http_port": "80",
    "use_sensor_calibration": "true",
    "configuration_connect_timeout": "0.5",
    "configuration_timeout": "1.0",
}
EXPECTED_PARAMETER_TYPES = {
    "sensor_port": "int",
    "http_port": "int",
    "use_sensor_calibration": "bool",
    "configuration_connect_timeout": "float",
    "configuration_timeout": "float",
}
ROS1_PARAMETER_TYPES = {
    **EXPECTED_PARAMETER_TYPES,
    "configuration_connect_timeout": "double",
    "configuration_timeout": "double",
}


def call_name(call):
    if isinstance(call.func, ast.Name):
        return call.func.id
    if isinstance(call.func, ast.Attribute):
        return call.func.attr
    return None


def keyword_value(call, name):
    return next(keyword.value for keyword in call.keywords if keyword.arg == name)


def string_value(node):
    assert isinstance(node, ast.Constant) and isinstance(node.value, str)
    return node.value


def type_name(node):
    assert isinstance(node, ast.Name)
    return node.id


def calls(tree, name):
    return [
        node for node in ast.walk(tree) if isinstance(node, ast.Call) and call_name(node) == name
    ]


def declared_arguments(tree):
    return {
        string_value(call.args[0]): string_value(keyword_value(call, "default_value"))
        for call in calls(tree, "DeclareLaunchArgument")
    }


def launch_parameter_types(tree):
    result = {}
    for call in calls(tree, "ParameterValue"):
        configuration = call.args[0]
        assert call_name(configuration) == "LaunchConfiguration"
        result[string_value(configuration.args[0])] = type_name(keyword_value(call, "value_type"))
    return result


def test_ros1_launch_exposes_typed_automatic_sensor_calibration_defaults():
    root = ElementTree.parse(ROOT / "launch/netft.launch").getroot()
    arguments = {argument.attrib["name"]: argument.attrib["default"] for argument in root.findall("arg")}
    assert {name: arguments.get(name) for name in EXPECTED_ARGUMENT_DEFAULTS} == (
        EXPECTED_ARGUMENT_DEFAULTS
    )
    parameters = {parameter.attrib["name"]: parameter for parameter in root.findall("./node/param")}
    assert {
        name: parameters[name].attrib.get("value") for name in EXPECTED_ARGUMENT_DEFAULTS
    } == {
        "sensor_ip": "$(arg sensor_ip)",
        **{
            name: "$(arg " + name + ")"
            for name in EXPECTED_PARAMETER_TYPES
        },
    }
    assert {
        name: parameters[name].attrib.get("type") if name in parameters else None
        for name in EXPECTED_PARAMETER_TYPES
    } == ROS1_PARAMETER_TYPES


def test_ros2_launch_passes_native_automatic_sensor_calibration_types():
    tree = ast.parse((ROOT / "launch/netft.launch.py").read_text(encoding="utf-8"))
    arguments = declared_arguments(tree)
    assert {name: arguments.get(name) for name in EXPECTED_ARGUMENT_DEFAULTS} == (
        EXPECTED_ARGUMENT_DEFAULTS
    )
    assert {
        name: launch_parameter_types(tree).get(name) for name in EXPECTED_PARAMETER_TYPES
    } == EXPECTED_PARAMETER_TYPES


def test_ros2_control_launch_exposes_xacro_connection_defaults():
    tree = ast.parse(
        (ROOT / "launch/netft_ros2_control.launch.py").read_text(encoding="utf-8")
    )
    arguments = declared_arguments(tree)
    assert {name: arguments.get(name) for name in EXPECTED_ARGUMENT_DEFAULTS} == (
        EXPECTED_ARGUMENT_DEFAULTS
    )
    values = {
        node.slice.value
        for node in ast.walk(tree)
        if isinstance(node, ast.Subscript)
        and isinstance(node.value, ast.Name)
        and node.value.id == "values"
        and isinstance(node.slice, ast.Constant)
        and isinstance(node.slice.value, str)
    }
    assert set(EXPECTED_ARGUMENT_DEFAULTS) <= values
