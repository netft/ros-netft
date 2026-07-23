from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_CONNECTION_PARAMETERS = {
    "sensor_ip": "192.168.1.1",
    "sensor_port": 49152,
    "http_port": 80,
    "use_sensor_calibration": True,
    "configuration_connect_timeout": 0.5,
    "configuration_timeout": 1.0,
}


def parse_yaml_mapping(path):
    root = {}
    stack = [(-1, root)]
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line:
            continue
        indent = len(line) - len(line.lstrip())
        key, separator, raw_value = line.strip().partition(":")
        assert separator, raw_line
        while indent <= stack[-1][0]:
            stack.pop()
        parent = stack[-1][1]
        value = raw_value.strip()
        if not value:
            child = {}
            parent[key] = child
            stack.append((indent, child))
        elif value == "true":
            parent[key] = True
        elif value == "false":
            parent[key] = False
        else:
            try:
                parent[key] = int(value)
            except ValueError:
                try:
                    parent[key] = float(value)
                except ValueError:
                    parent[key] = value.strip('"\'')
    return root


def assert_connection_defaults(parameters):
    assert {name: parameters.get(name) for name in EXPECTED_CONNECTION_PARAMETERS} == (
        EXPECTED_CONNECTION_PARAMETERS
    )
    assert isinstance(parameters.get("sensor_port"), int)
    assert isinstance(parameters.get("http_port"), int)
    assert isinstance(parameters.get("use_sensor_calibration"), bool)
    assert isinstance(parameters.get("configuration_connect_timeout"), float)
    assert isinstance(parameters.get("configuration_timeout"), float)
    assert parameters.get("counts_per_force") == 1000000.0
    assert parameters.get("counts_per_torque") == 1000000.0


def test_ros1_yaml_exposes_automatic_sensor_calibration_defaults():
    assert_connection_defaults(parse_yaml_mapping(ROOT / "config/netft_ros1.yaml"))


def test_ros2_yaml_exposes_automatic_sensor_calibration_defaults():
    config = parse_yaml_mapping(ROOT / "config/netft_ros2.yaml")
    assert_connection_defaults(config["netft"]["ros__parameters"])


def test_ros2_control_yaml_contains_only_controller_configuration():
    config = parse_yaml_mapping(ROOT / "config/netft_ros2_control.yaml")
    assert set(config) == {"controller_manager", "netft_broadcaster"}
    assert "netft_hardware" not in config
    assert config["controller_manager"]["ros__parameters"]["netft_broadcaster"] == {
        "type": "force_torque_sensor_broadcaster/ForceTorqueSensorBroadcaster"
    }
    assert config["netft_broadcaster"]["ros__parameters"]["sensor_name"] == (
        "netft_sensor"
    )
