import ast
import re
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_URL = "https://github.com/han-xudong/ros-netft"
BUGTRACKER_URL = REPOSITORY_URL + "/issues"


def _manifest_root():
    return ElementTree.parse(str(ROOT / "package.xml")).getroot()


def _setup_keywords():
    tree = ast.parse((ROOT / "setup.py").read_text(encoding="utf-8"))
    setup_call = next(
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "setup"
    )
    return {
        keyword.arg: ast.literal_eval(keyword.value)
        for keyword in setup_call.keywords
        if keyword.arg is not None
        and not isinstance(keyword.value, ast.Call)
    }


def _pixi_workspace_version():
    text = (ROOT / "pixi.toml").read_text(encoding="utf-8")
    match = re.search(r'^version = "([0-9]+\.[0-9]+\.[0-9]+)"$', text, re.MULTILINE)
    assert match is not None
    return match.group(1)


def test_release_versions_are_identical():
    manifest_version = _manifest_root().findtext("version")
    assert manifest_version == "0.1.0"
    assert _setup_keywords()["version"] == manifest_version
    assert _pixi_workspace_version() == manifest_version


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

    setup = _setup_keywords()
    assert setup["url"] == REPOSITORY_URL
    assert setup["maintainer"] == "Xudong Han"
    assert setup["maintainer_email"] == "hanxudong159@126.com"
    assert setup["project_urls"] == {"Bug Tracker": BUGTRACKER_URL}


def test_unused_ament_python_resource_marker_is_absent():
    assert not (ROOT / "resource" / "netft_driver").exists()


def test_cmake_installs_only_native_launch_and_config_files():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "DIRECTORY launch config" not in text
    assert "FILES launch/netft.launch" in text
    assert "FILES config/netft_ros1.yaml" in text
    assert "FILES launch/netft.launch.py" in text
    assert "FILES config/netft_ros2.yaml" in text


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


def test_ros2_shutdown_regression_is_integrated_into_smoke():
    smoke = (ROOT / "test/integration/ros2_smoke.sh").read_text(encoding="utf-8")
    assert not (ROOT / "test/integration/ros2_shutdown.sh").exists()
    assert "run_full_graph_scenario" in smoke
    assert "run_shutdown_scenario" in smoke
    assert "[2, 66, 2, 0]" in smoke
    assert "[2, 0]" in smoke
    assert "ExternalShutdownException" in smoke
    assert smoke.count("-p receive_timeout:=1.0") == 2


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
    assert "bash test/integration/ros1_smoke.sh" in workflow
    assert "bash test/integration/ros2_smoke.sh" in workflow
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
    assert "import netft_driver" in smoke
    assert "ROS 1 installed Python module:" in smoke
    assert smoke.count('PYTHONPATH="$repo_root"') == 1


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
    assert "at-least-one" in build_script
    assert "exactly-one" in build_script
