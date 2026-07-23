import os
import re
import subprocess
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
DEPENDENCY_INPUT_NAMES = {
    "Cargo.toml",
    "Pipfile",
    "conanfile.py",
    "conanfile.txt",
    "package.json",
    "package.xml",
    "pixi.lock",
    "pixi.toml",
    "pyproject.toml",
    "setup.cfg",
    "setup.py",
    "vcpkg.json",
}
DEPENDENCY_INPUT_SUFFIXES = {
    ".cmake",
    ".repos",
}
EXCLUDED_INPUT_DIRECTORY_NAMES = {
    ".cache",
    ".git",
    ".mypy_cache",
    ".nox",
    ".pixi",
    ".pytest_cache",
    ".ruff_cache",
    ".superpowers",
    ".tox",
    ".venv",
    ".worktrees",
    "CMakeFiles",
    "__pycache__",
    "_deps",
    "build",
    "install",
    "log",
    "node_modules",
    "venv",
}


def is_dependency_input(path):
    is_workflow = (
        len(path.parts) == 3
        and path.parts[:2] == (".github", "workflows")
        and path.suffix in {".yml", ".yaml"}
    )
    is_manifest = (
        path.name in DEPENDENCY_INPUT_NAMES
        or path.name.startswith("requirements")
        and path.suffix == ".txt"
    )
    return (
        path.name == "CMakeLists.txt"
        or path.suffix in DEPENDENCY_INPUT_SUFFIXES
        or is_workflow
        or is_manifest
    )


def filesystem_repository_paths(root):
    for directory, directory_names, file_names in os.walk(root):
        directory_names[:] = sorted(
            name
            for name in directory_names
            if name not in EXCLUDED_INPUT_DIRECTORY_NAMES
        )
        directory_path = Path(directory)
        for name in sorted(file_names):
            yield (directory_path / name).relative_to(root)


def repository_dependency_inputs(root=ROOT):
    root = Path(root)
    try:
        result = subprocess.run(
            [
                "git",
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            cwd=root,
            check=False,
            capture_output=True,
        )
    except OSError:
        result = None

    if result is not None and result.returncode == 0:
        paths = (
            Path(os.fsdecode(relative))
            for relative in result.stdout.split(b"\0")
            if relative
        )
    else:
        paths = filesystem_repository_paths(root)

    for relative in paths:
        path = Path(relative)
        if is_dependency_input(path):
            yield path


def test_dependency_input_discovery_falls_back_outside_git(tmp_path):
    expected = {
        Path("CMakeLists.txt"),
        Path("nested/CMakeLists.txt"),
        Path("nested/toolchain.cmake"),
        Path("nested/dependencies.repos"),
        Path(".github/workflows/ci.yml"),
        Path("package.xml"),
        Path("nested/requirements-dev.txt"),
        Path("nested/pixi.toml"),
    }
    excluded = {
        Path(directory) / "CMakeLists.txt"
        for directory in (
            ".git",
            ".pixi",
            ".worktrees",
            ".superpowers",
            "build",
            "install",
            "log",
            "__pycache__",
            ".pytest_cache",
        )
    }
    for relative in expected | excluded | {Path("nested/README.md")}:
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("dependency input\n", encoding="utf-8")

    assert set(repository_dependency_inputs(tmp_path)) == expected


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
    assert ("buildtool_depend", "ros_environment", None) in dependencies
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


def test_repository_keeps_one_private_vendored_core():
    assert (ROOT / "src/core/UPSTREAM").is_file()
    assert (ROOT / "src/core/LICENSE").is_file()

    for source in ("client.cpp", "protocol.cpp", "status.cpp"):
        assert not (ROOT / "src" / source).exists()

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "add_library(netft_core STATIC ${NETFT_SNAPSHOT_SOURCES})" in cmake
    assert "src/core/src/client.cpp" in cmake
    assert "netft_" + "sdk_core" not in cmake
    assert not re.search(r"install\s*\(\s*TARGETS\s+netft_core\b", cmake)
    assert not re.search(
        r"catkin_package\s*\([^)]*\bLIBRARIES\s+netft_core\b", cmake, re.DOTALL
    )
    assert not re.search(
        r"install\s*\(\s*DIRECTORY\s+(?:include/netft|src/core)", cmake
    )

    forbidden_coupling = re.compile(
        r"find_" r"package\s*\(\s*netft\b|"
        r"Fetch" r"Content|External" r"Project|"
        r"netft[-_]" r"cpp|netft_" r"sdk_core",
        re.IGNORECASE,
    )
    violations = {
        str(path): match.group(0)
        for path in repository_dependency_inputs()
        if (
            match := forbidden_coupling.search(
                (ROOT / path).read_text(encoding="utf-8")
            )
        )
    }
    assert violations == {}

    manifest = ElementTree.parse(str(ROOT / "package.xml")).getroot()
    assert all(
        not (element.tag.endswith("depend") and (element.text or "").strip() == "netft")
        for element in manifest
    )
    gitmodules = ROOT / ".gitmodules"
    assert not gitmodules.exists() or "netft" not in gitmodules.read_text(
        encoding="utf-8"
    ).lower()
