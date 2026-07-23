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
ROS2_CONTROL_HARDWARE_ARGUMENT_DEFAULTS = {
    "sensor_name": "netft_sensor",
    **EXPECTED_ARGUMENT_DEFAULTS,
    "counts_per_force": "1000000",
    "counts_per_torque": "1000000",
    "receive_timeout": "0.1",
    "activation_timeout": "2.0",
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


def native_string_value_names(tree):
    assignment = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_nodes"
        for node in node.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "values"
            for target in node.targets
        )
    )
    assert isinstance(assignment.value, ast.DictComp)
    assert len(assignment.value.generators) == 1
    generator = assignment.value.generators[0]
    assert isinstance(generator.target, ast.Name) and generator.target.id == "name"
    assert isinstance(generator.iter, ast.Tuple)
    assert call_name(assignment.value.value) == "perform"
    configuration = assignment.value.value.func.value
    assert isinstance(configuration, ast.Call)
    assert call_name(configuration) == "LaunchConfiguration"
    assert isinstance(configuration.args[0], ast.Name)
    assert configuration.args[0].id == "name"
    return {string_value(item) for item in generator.iter.elts}


def subscript_string_value(node):
    value = node.slice
    if isinstance(value, ast.Index):
        value = value.value
    if isinstance(value, ast.Constant) and isinstance(value.value, str):
        return value.value
    return None


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


def test_ros2_control_launch_passes_native_strings_for_every_hardware_argument():
    tree = ast.parse(
        (ROOT / "launch/netft_ros2_control.launch.py").read_text(encoding="utf-8")
    )
    arguments = declared_arguments(tree)
    assert {
        name: arguments.get(name)
        for name in ROS2_CONTROL_HARDWARE_ARGUMENT_DEFAULTS
    } == ROS2_CONTROL_HARDWARE_ARGUMENT_DEFAULTS
    assert native_string_value_names(tree) == set(
        ROS2_CONTROL_HARDWARE_ARGUMENT_DEFAULTS
    )
    values = {
        subscript_string_value(node)
        for node in ast.walk(tree)
        if isinstance(node, ast.Subscript)
        and isinstance(node.value, ast.Name)
        and node.value.id == "values"
        and subscript_string_value(node) is not None
    }
    assert values == set(ROS2_CONTROL_HARDWARE_ARGUMENT_DEFAULTS)
