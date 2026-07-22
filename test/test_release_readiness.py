import re
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_URL = "https://github.com/netft/ros-netft"
BUGTRACKER_URL = REPOSITORY_URL + "/issues"


def _manifest_root():
    return ElementTree.parse(str(ROOT / "package.xml")).getroot()


def _pixi_workspace_version():
    text = (ROOT / "pixi.toml").read_text(encoding="utf-8")
    match = re.search(r'^version = "([0-9]+\.[0-9]+\.[0-9]+)"$', text, re.MULTILINE)
    assert match is not None
    return match.group(1)


def test_release_versions_are_identical():
    manifest_version = _manifest_root().findtext("version")
    assert manifest_version == "0.2.1"
    assert _pixi_workspace_version() == manifest_version
    changelog = (ROOT / "CHANGELOG.rst").read_text(encoding="utf-8")
    assert "0.2.1 (2026-07-22)" in changelog
    assert "0.2.0 (2026-07-21)" in changelog
    assert changelog.index("0.2.1 (2026-07-22)") < changelog.index(
        "0.2.0 (2026-07-21)"
    )


def test_readme_documents_the_standalone_and_ros2_control_contracts():
    text = (ROOT / "README.md").read_text(encoding="utf-8")

    for required in (
        "Standalone driver",
        "ros2_control",
        "netft_driver/NetFTHardwareInterface",
        "urdf/netft.ros2_control.xacro",
        "config/netft_ros2_control.yaml",
        "--param-file /path/to/controllers.yaml",
        "/<encoded sensor token>/bias",
        "fail-stop",
        "lifecycle",
        "NaN",
        "read()",
        "ERROR",
        "force.x",
        "force.y",
        "force.z",
        "torque.x",
        "torque.y",
        "torque.z",
    ):
        assert required in text

    warning = text[text.index("> **Warning:** Software bias") :]
    for required in ("stationary", "unloaded", "safe", "does not acknowledge"):
        assert required in warning

    support_rows = [
        "ROS 2 Lyrical",
        "ROS 2 Kilted",
        "ROS 2 Jazzy",
        "ROS 2 Humble",
        "ROS 2 Rolling",
        "ROS 1 Noetic*",
    ]
    positions = [text.index(row) for row in support_rows]
    assert positions == sorted(positions)


def test_readme_documents_the_complete_fail_stop_recovery_sequence():
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    start = text.index("The plugin uses a fail-stop recovery policy.")
    end = text.index("On Humble,", start)
    recovery = text[start:end]
    normalized_recovery = " ".join(recovery.split())

    expected_states = ("active", "inactive", "unconfigured", "inactive", "active")
    positions = []
    for state in expected_states:
        positions.append(recovery.index(state, positions[-1] + 1 if positions else 0))
    assert positions == sorted(positions)
    assert "cleanup" in normalized_recovery
    assert "configure" in normalized_recovery
    assert "skip" in normalized_recovery
    assert "not applicable" in normalized_recovery
    assert "fatal fault is cleared during configure" in normalized_recovery

    commands = [
        "ros2 control set_hardware_component_state wrist_netft_hardware inactive",
        "ros2 control set_hardware_component_state wrist_netft_hardware unconfigured",
        "ros2 control set_hardware_component_state wrist_netft_hardware inactive",
        "ros2 control set_hardware_component_state wrist_netft_hardware active",
    ]
    command_positions = []
    for command in commands:
        command_positions.append(
            recovery.index(command, command_positions[-1] + 1 if command_positions else 0)
        )
    assert command_positions == sorted(command_positions)


def test_default_ros2_control_names_use_the_documented_injective_encoding():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    architecture = (ROOT / "docs/architecture.md").read_text(encoding="utf-8")
    config = (ROOT / "config/netft_ros2_control.yaml").read_text(encoding="utf-8")
    implementation = (ROOT / "src/netft_hardware_interface.cpp").read_text(
        encoding="utf-8"
    )

    for text in (readme, architecture, config):
        assert "netft_encoded_" in text
        assert "lowercase hexadecimal UTF-8 bytes" in text
    for required in (
        "ROS-valid token",
        "reserved prefix",
        "explicit `bias_service`",
    ):
        assert required in readme
    assert "ros_name_token_" in implementation
    assert '"/" + ros_name_token_ + "/bias"' in implementation
    assert "auxiliary_node_name(ros_name_token_)" in implementation


def test_architecture_describes_only_the_native_runtime():
    text = (ROOT / "docs/architecture.md").read_text(encoding="utf-8")

    for required in (
        "netft_core",
        "src/protocol.cpp",
        "src/status.cpp",
        "src/client.cpp",
        "src/ros1_node.cpp",
        "src/ros2_node.cpp",
        "src/netft_hardware_interface.cpp",
        "RealtimeBuffer",
        "Reconnect",
        "FailStop",
        "Test boundaries",
    ):
        assert required in text

    for obsolete in (
        "protocol.py",
        "status.py",
        "client.py",
        "node_common.py",
        "rclpy",
        "rospy",
        "Python runtime",
        "Pure pytest covers encoding",
        "The installed\nThe native",
    ):
        assert obsolete not in text


def test_public_project_metadata_is_complete():
    root = _manifest_root()
    urls = {
        (item.attrib.get("type"), (item.text or "").strip())
        for item in root.findall("url")
    }
    assert ("website", REPOSITORY_URL) in urls
    assert ("repository", REPOSITORY_URL) in urls
    assert ("bugtracker", BUGTRACKER_URL) in urls
    author = root.find("author")
    assert author is not None
    assert author.text == "Xudong Han"
    assert author.attrib["email"] == "hanxudong159@126.com"

def test_unused_ament_python_resource_marker_is_absent():
    assert not (ROOT / "resource" / "netft_driver").exists()


def test_cmake_installs_only_native_launch_and_config_files():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "DIRECTORY launch config" not in text
    assert "FILES launch/netft.launch" in text
    assert "FILES config/netft_ros1.yaml" in text
    assert "FILES launch/netft.launch.py" in text
    assert "FILES config/netft_ros2.yaml" in text
    assert "ament_cmake_python" not in text
    assert "ament_python_install_package" not in text
    assert "catkin_python_setup" not in text
    assert "catkin_install_python" not in text
    assert "PROGRAMS scripts/netft_node scripts/netft_check" not in text
    assert "set_target_properties(netft_node_cpp PROPERTIES OUTPUT_NAME netft_node)" in text
    assert "install(TARGETS netft_node_cpp netft_check" in text


def test_private_ros2_test_tools_are_installed_only_for_integration_runs():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    build_script = (ROOT / "test/integration/build_package.sh").read_text(
        encoding="utf-8"
    )
    opt_in_scripts = (
        ROOT / "test/integration/ros2_smoke.sh",
        ROOT / "test/integration/ros2_control_smoke.sh",
        ROOT / "test/integration/ros2_control_isolation_test.sh",
    )

    assert 'option(NETFT_INSTALL_TEST_TOOLS "Install private integration-test tools" OFF)' in cmake
    guarded_install = cmake.split("if(NETFT_INSTALL_TEST_TOOLS)", 1)[1].split(
        "endif()", 1
    )[0]
    for private_asset in (
        "test/integration/ros2_control_smoke.sh",
        "test/integration/ros2_domain_lease.sh",
        "test/integration/ros2_graph_probe.py",
        "test/integration/fake_sensor_process.py",
        "test/support/fake_sensor.py",
    ):
        assert private_asset in guarded_install

    assert 'test ! -e "$share_root/test"' in build_script
    assert "NETFT_INSTALL_TEST_TOOLS=ON" not in build_script
    for script in opt_in_scripts:
        assert "-DNETFT_INSTALL_TEST_TOOLS=ON" in script.read_text(encoding="utf-8")


def test_python_runtime_has_been_removed_from_the_source_tree():
    assert not (ROOT / "setup.py").exists()
    assert not (ROOT / "scripts" / "netft_node").exists()
    assert not (ROOT / "scripts" / "netft_check").exists()
    assert not (ROOT / "netft_driver").exists()


def test_non_hardware_tests_are_registered_for_both_build_types():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    manifest = (ROOT / "package.xml").read_text(encoding="utf-8")
    pixi = (ROOT / "pixi.toml").read_text(encoding="utf-8")
    pytest_config = (ROOT / "pytest.ini").read_text(encoding="utf-8")

    assert "catkin_run_tests_target(" in cmake
    assert cmake.count('COMMAND "${CMAKE_COMMAND} -E env ') == 2
    assert "ament_cmake_pytest" in cmake
    assert "ament_add_pytest_test(netft_unit test" in cmake
    assert "ENV NETFT_HARNESS=ros2" in cmake
    assert "netft_ros1_smoke_harness" in cmake
    assert "netft_ros2_smoke_harness" in cmake
    assert (ROOT / "test/test_shell_harness.py").is_file()
    assert '<test_depend condition="$ROS_VERSION == 2">ament_cmake_pytest</test_depend>' in manifest
    assert 'ros1-harness = "bash test/integration/ros1_smoke_harness_test.sh"' in pixi
    assert 'ros2-harness = "bash test/integration/ros2_smoke_harness_test.sh"' in pixi
    assert 'addopts = -m "not hardware"' in pytest_config
    assert "hardware: requires an explicitly authorized real sensor" in pytest_config


def test_ros2_targets_use_exported_cmake_targets_supported_by_rolling():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "ament_target_dependencies(" not in cmake
    for dependency in ("rclcpp", "geometry_msgs", "std_srvs", "diagnostic_msgs"):
        assert "${" + dependency + "_TARGETS}" in cmake


def test_ros2_shutdown_regression_is_integrated_into_smoke():
    smoke = (ROOT / "test/integration/ros2_smoke.sh").read_text(encoding="utf-8")
    assert not (ROOT / "test/integration/ros2_shutdown.sh").exists()
    assert "run_full_graph_scenario" in smoke
    assert "run_shutdown_scenario" in smoke
    assert "[2, 66, 2, 0]" in smoke
    assert "[2, 0]" in smoke
    assert "ExternalShutdownException" in smoke
    assert smoke.count("-p receive_timeout:=0.8") == 1
    assert smoke.count("-p receive_timeout:=1.0") == 1


def test_ros2_graph_smoke_uses_one_installed_rclpy_probe():
    smoke = (ROOT / "test/integration/ros2_smoke.sh").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    manifest = (ROOT / "package.xml").read_text(encoding="utf-8")

    assert (ROOT / "test/integration/ros2_graph_probe.py").is_file()
    assert "PROGRAMS test/integration/ros2_graph_probe.py" in cmake
    assert 'installed_probe="$temp_root/ws/install/netft_driver/share/netft_driver/' in smoke
    assert smoke.count('python3 "$installed_probe"') == 2
    assert 'python3 "$installed_probe" full' in smoke
    assert 'python3 "$installed_probe" ready' in smoke
    assert '<test_depend condition="$ROS_VERSION == 2">rclpy</test_depend>' in manifest
    for obsolete in (
        "ros2 node",
        "ros2 service",
        "ros2 topic",
        "--no-daemon",
        "--spin-time",
        "service_has_type",
    ):
        assert obsolete not in smoke


def test_ros2_control_spawners_receive_controller_parameters_explicitly():
    smoke = (ROOT / "test/integration/ros2_control_smoke.sh").read_text(
        encoding="utf-8"
    )
    launch = (ROOT / "launch/netft_ros2_control.launch.py").read_text(
        encoding="utf-8"
    )

    assert '--param-file "$smoke_root/controllers.yaml"' in smoke
    assert '"--param-file"' in launch
    assert 'f"{share}/config/netft_ros2_control.yaml"' in launch


def test_ros2_control_smoke_uses_an_installed_rclpy_probe_with_hard_cli_bounds():
    smoke = (ROOT / "test/integration/ros2_control_smoke.sh").read_text(
        encoding="utf-8"
    )

    assert 'installed_probe="$installed_share/test/integration/ros2_graph_probe.py"' in smoke
    assert 'python3 "$installed_probe"' in smoke
    normalized_smoke = " ".join(smoke.replace("\\\n", " ").split())
    pre_fault_probe = (
        "run_control_probe control --service-name /left_ft/bias "
        "--service-name /right_ft/bias "
        "--wrench-topic /left_broadcaster/wrench "
        "--wrench-topic /right_broadcaster/wrench "
        "--bias-service /right_ft/bias "
        "--bias-wrench-topic /right_broadcaster/wrench"
    )
    assert pre_fault_probe in normalized_smoke
    assert "--bias-service /left_ft/bias" not in smoke
    post_fault_probe = (
        "run_control_probe control --service-name /right_ft/bias "
        "--wrench-topic /right_broadcaster/wrench"
    )
    assert post_fault_probe in normalized_smoke
    success_message = 'echo "right broadcaster published a new wrench after left timeout"'
    assert normalized_smoke.index(success_message) > normalized_smoke.index(
        post_fault_probe
    )
    for forbidden in (
        "ros2 service list",
        "ros2 topic echo",
        "ros2 service call",
        "ros2 topic list",
    ):
        assert forbidden not in smoke

    assert "timeout --kill-after=" in smoke
    assert 'timeout --kill-after=2s 15s ros2 run controller_manager spawner' in smoke
    assert 'timeout --kill-after=2s 5s ros2 control list_hardware_components' in smoke
    assert "component_state()" in smoke
    assert "timeout --kill-after=2s 5s ros2 control list_hardware_components" in smoke


def test_ros2_smokes_lease_isolated_domains_and_clean_up_daemons():
    standalone = (ROOT / "test/integration/ros2_smoke.sh").read_text(
        encoding="utf-8"
    )
    control = (ROOT / "test/integration/ros2_control_smoke.sh").read_text(
        encoding="utf-8"
    )
    helper = ROOT / "test/integration/ros2_domain_lease.sh"
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    pixi = (ROOT / "pixi.toml").read_text(encoding="utf-8")
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    cleanup_harness = ROOT / "test/integration/ros2_control_cleanup_harness_test.sh"
    isolation_harness = ROOT / "test/integration/ros2_control_isolation_test.sh"

    assert helper.is_file()
    for smoke in (standalone, control):
        assert "acquire_ros2_domain" in smoke
        assert "release_ros2_domain_leases" in smoke
        assert "stop_ros2_daemon_for_domain" in smoke
        assert "export ROS_LOCALHOST_ONLY=1" in smoke
    assert standalone.count("acquire_ros2_domain") == 2
    assert control.count("acquire_ros2_domain") == 1
    assert "robot_state_publisher.log controller_manager.log" in control
    assert 'cat "$smoke_root/$log"' in control
    assert "trap cleanup EXIT" in control
    assert "trap 'exit 130' INT" in control
    assert "trap 'exit 143' TERM" in control
    assert "if (( status == 0 )); then" in standalone
    assert "if ((status == 0)); then" in control
    assert "ros2_domain_lease.sh" in cmake
    assert "netft_ros2_domain_lease_harness" in cmake
    assert "netft_ros2_control_cleanup_harness" in cmake
    assert cleanup_harness.is_file()
    assert isolation_harness.is_file()
    assert "ROS 2 control smoke leased domain:" in control
    assert "cleanup-failure-success" in cleanup_harness.read_text(encoding="utf-8")
    assert "cleanup-failure-original" in cleanup_harness.read_text(encoding="utf-8")
    assert "signal-int INT 130" in cleanup_harness.read_text(encoding="utf-8")
    assert "signal-term TERM 143" in cleanup_harness.read_text(encoding="utf-8")
    isolation = isolation_harness.read_text(encoding="utf-8")
    assert "assert_domain_has_no_daemon" in isolation
    assert "domain_a" in isolation and "domain_b" in isolation
    assert "ros2_control two-instance smoke passed" in isolation
    assert "timeout --kill-after=10s 180s colcon build" in isolation
    assert "timeout --foreground --signal=TERM --kill-after=20s 120s" in isolation
    assert "terminate_active_pid" in isolation
    assert 'active_pids[$index]=""' in isolation
    assert "bash test/integration/ros2_control_isolation_test.sh" in workflow
    assert 'if [[ "${ROS_DISTRO}" == "jazzy" ]]' in workflow
    assert "flock-check = \"bash -c 'command -v flock >/dev/null && flock --version'\"" in pixi
    assert "util-linux" in workflow
    assert "procps" in workflow



def test_native_ci_has_the_supported_source_matrix_without_release_claims():
    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    for distro in (
        "noetic",
        "humble",
        "jazzy",
        "kilted",
        "lyrical",
        "rolling",
    ):
        assert "distro: " + distro in workflow
        assert "ros:" + distro + "-ros-base" in workflow
    assert "policy: legacy-eol-source-only" in workflow
    assert "policy: development" in workflow
    assert 'rosdep update --rosdistro "${ROS_DISTRO}" --include-eol-distros' in workflow
    assert "rosdep install --from-paths . --ignore-src -r -y" in workflow
    assert "apt-get install -y python3-pytest" in workflow
    assert re.search(r"actions/checkout@[0-9a-f]{40} # v7", workflow)
    assert "cmake -S . -B build/core-only" in workflow
    assert "cmake --build build/core-only" in workflow
    assert "(cd build/core-only && ctest --output-on-failure)" in workflow
    assert "Total Tests:" in workflow
    assert "bash test/integration/ros1_smoke.sh" in workflow
    assert "bash test/integration/ros2_smoke.sh" in workflow
    assert "bash test/integration/ros2_control_smoke.sh" in workflow
    assert "192.168.31.100" not in workflow
    assert "bloom-release" not in workflow
    assert "docker build" not in workflow


def test_hardware_acceptance_report_is_not_public():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    assert not (ROOT / "docs/hardware-validation.md").exists()
    assert "docs/hardware-validation.md" not in readme


def test_readme_noetic_instructions_refresh_eol_rosdep_metadata():
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    start = text.index("### ROS 1 Noetic legacy source installation")
    end = text.index("## Quick start")
    instructions = text[start:end]

    update = "rosdep update --include-eol-distros"
    install = "rosdep install --from-paths src --ignore-src -r -y"
    assert update in instructions
    assert instructions.index(update) < instructions.index(install)


def test_readme_commands_match_installed_interfaces():
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    ros1_launch = (ROOT / "launch/netft.launch").read_text(encoding="utf-8")
    ros2_launch = (ROOT / "launch/netft.launch.py").read_text(encoding="utf-8")
    for command in (
        "ros2 launch netft_driver netft.launch.py "
        "sensor_ip:=192.168.31.100 sensor_port:=49152",
        "roslaunch netft_driver netft.launch "
        "sensor_ip:=192.168.31.100 sensor_port:=49152",
        "ros2 run netft_driver netft_check",
        "rosrun netft_driver netft_check",
        "ros2 topic echo --once /netft/wrench",
        "rostopic echo -n 1 /netft/wrench",
    ):
        assert command in text

    assert '<arg name="sensor_ip" default="192.168.31.100"/>' in ros1_launch
    assert '<arg name="sensor_port" default="49152"/>' in ros1_launch
    assert 'DeclareLaunchArgument("sensor_ip", default_value="192.168.31.100")' in ros2_launch
    assert 'DeclareLaunchArgument("sensor_port", default_value="49152")' in ros2_launch


def test_integration_scripts_require_python3_without_a_ci_alias():
    scripts = sorted((ROOT / "test/integration").glob("*.sh"))
    bare_python = re.compile(r"(?<![A-Za-z0-9_])python(?=[ \t])")
    offenders = {
        path.relative_to(ROOT).as_posix(): [
            number
            for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1
            )
            if bare_python.search(line)
        ]
        for path in scripts
    }
    offenders = {path: lines for path, lines in offenders.items() if lines}
    assert offenders == {}

    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    assert "python-is-python3" not in workflow


def test_ros1_graph_smoke_uses_the_installed_package():
    smoke = (ROOT / "test/integration/ros1_smoke.sh").read_text(encoding="utf-8")

    assert "catkin_make -DCMAKE_BUILD_TYPE=Release install" in smoke
    assert "source install/setup.bash" in smoke
    assert "source devel/setup.bash" not in smoke
    assert "rospack find netft_driver" in smoke
    assert 'expected_package_path="$temp_root/ws/install/share/netft_driver"' in smoke
    assert "ROS 1 installed package path:" in smoke
    assert 'rosrun netft_driver netft_node ' in smoke
    assert 'rosrun netft_driver netft_check --help' in smoke
    assert "ROS 1 installed native executable:" in smoke
    assert "ROS 1 installed Python module:" not in smoke
    assert "import netft_driver" not in smoke
    assert smoke.count('PYTHONPATH="$repo_root"') == 1


def test_ros2_graph_smoke_uses_installed_native_entrypoints_only():
    smoke = (ROOT / "test/integration/ros2_smoke.sh").read_text(encoding="utf-8")

    assert "ros2 pkg executables netft_driver" in smoke
    assert 'ros2 run netft_driver netft_node ' in smoke
    assert 'ros2 run netft_driver netft_check --help' in smoke
    assert "site-packages/netft_driver" in smoke


def test_package_build_validates_executed_junit_counts():
    validator = ROOT / "test/integration/validate_junit.py"
    build_script = (ROOT / "test/integration/build_package.sh").read_text(
        encoding="utf-8"
    )

    assert validator.is_file()
    assert "validate_junit.py" in build_script
    assert build_script.count("netft_unit") >= 2
    assert "netft_ros1_smoke_harness" in build_script
    assert "netft_ros2_smoke_harness" in build_script
    assert "netft_ros1_node" in build_script
    assert "roscore -p" in build_script
    assert "netft_ros2_node" in build_script
    assert "netft_hardware_interface" in build_script
    assert "at-least-one" in build_script
    assert "exactly-one" in build_script
