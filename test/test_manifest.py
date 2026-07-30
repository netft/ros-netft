import re
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]


def test_manifest_declares_dual_ros_build_contract():
    root = ElementTree.parse(str(ROOT / "package.xml")).getroot()
    dependencies = {
        (element.tag, element.text.strip(), element.attrib.get("condition"))
        for element in root
        if element.tag.endswith("depend")
    }
    build_types = {
        (item.text.strip(), item.attrib.get("condition"))
        for item in root.findall("./export/build_type")
    }

    assert root.attrib["format"] == "3"
    assert root.findtext("name") == "netft_driver"
    assert ("buildtool_depend", "catkin", "$ROS_VERSION == 1") in dependencies
    assert ("buildtool_depend", "ament_cmake", "$ROS_VERSION == 2") in dependencies
    assert build_types == {
        ("catkin", "$ROS_VERSION == 1"),
        ("ament_cmake", "$ROS_VERSION == 2"),
    }


def test_private_core_provenance_is_structurally_valid():
    metadata = dict(
        line.split("=", 1)
        for line in (ROOT / "src/core/UPSTREAM").read_text(encoding="utf-8").splitlines()
        if line
    )

    assert metadata["repository"] == "https://github.com/netft/netft-cpp.git"
    assert re.fullmatch(r"v\d+\.\d+\.\d+", metadata["tag"])
    assert re.fullmatch(r"[0-9a-f]{40}", metadata["commit"])
    assert (ROOT / "src/core/LICENSE").is_file()
