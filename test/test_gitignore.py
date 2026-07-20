import os
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def _verbose_check_ignore_result_is_ignored(returncode, output):
    if returncode == 1:
        return False
    fields = output.split(b"\0")
    if returncode != 0 or len(fields) < 4:
        raise ValueError("invalid git check-ignore result")
    return not fields[2].startswith(b"!")


def test_verbose_check_ignore_result_distinguishes_negated_patterns():
    negated = b".gitignore\x0073\x00!.env.example\x00.env.example\x00"
    ignored = b".gitignore\x0072\x00.env.*\x00.env.local\x00"

    assert not _verbose_check_ignore_result_is_ignored(0, negated)
    assert _verbose_check_ignore_result_is_ignored(0, ignored)
    assert not _verbose_check_ignore_result_is_ignored(1, b"")


def make_git_environment(home, xdg_config, template):
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    env.update(
        {
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_TEMPLATE_DIR": str(template),
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(xdg_config),
        }
    )
    return env


def make_policy_checker(base_path):
    base_path.mkdir()
    repo = base_path / "repo"
    repo.mkdir()
    home = base_path / "home"
    home.mkdir()
    xdg_config = base_path / "xdg-config"
    xdg_config.mkdir()
    template = base_path / "git-template"
    template.mkdir()
    environment = make_git_environment(home, xdg_config, template)

    init_result = subprocess.run(
        ["git", "init", "--quiet", "--template={}".format(template)],
        cwd=str(repo),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        env=environment,
        check=False,
    )
    if init_result.returncode != 0:
        pytest.fail("git init failed: {}".format(init_result.stderr))

    shutil.copyfile(str(ROOT / ".gitignore"), str(repo / ".gitignore"))
    info = repo / ".git" / "info"
    info.mkdir(parents=True, exist_ok=True)
    (info / "exclude").write_text("", encoding="utf-8")

    def check(path):
        result = subprocess.run(
            [
                "git",
                "check-ignore",
                "--no-index",
                "--verbose",
                "-z",
                "--stdin",
            ],
            cwd=str(repo),
            input=path.encode("utf-8") + b"\0",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            check=False,
        )
        if result.returncode not in (0, 1):
            pytest.fail(
                "git check-ignore failed for {!r}: {}".format(
                    path, result.stderr.decode("utf-8", errors="replace")
                )
            )
        return _verbose_check_ignore_result_is_ignored(
            result.returncode, result.stdout
        )

    return check


def test_policy_repository_discards_hostile_git_environment(tmp_path, monkeypatch):
    hostile_root = tmp_path / "hostile"
    hostile_environment = {
        "GIT_DIR": hostile_root / "git-dir",
        "GIT_WORK_TREE": hostile_root / "work-tree",
        "GIT_COMMON_DIR": hostile_root / "common-dir",
        "GIT_TEMPLATE_DIR": hostile_root / "template-dir",
        "GIT_OBJECT_DIRECTORY": hostile_root / "objects",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES": hostile_root / "alternate-objects",
        "GIT_INDEX_FILE": hostile_root / "index",
        "GIT_SHALLOW_FILE": hostile_root / "shallow",
        "GIT_CONFIG_GLOBAL": hostile_root / "global-config",
        "GIT_CONFIG_SYSTEM": hostile_root / "system-config",
        "GIT_CONFIG_COUNT": "1",
        "GIT_CONFIG_KEY_0": "core.excludesFile",
        "GIT_CONFIG_VALUE_0": str(hostile_root / "global-excludes"),
    }
    for key, value in hostile_environment.items():
        monkeypatch.setenv(key, str(value))

    controlled_environment = make_git_environment(
        tmp_path / "controlled-home",
        tmp_path / "controlled-xdg",
        tmp_path / "controlled-template",
    )
    assert {
        key for key in controlled_environment if key.startswith("GIT_")
    } == {"GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_TEMPLATE_DIR"}
    assert all(
        str(hostile_root) not in value for value in controlled_environment.values()
    )

    is_ignored = make_policy_checker(tmp_path / "policy")

    assert is_ignored(".tools/bin/tool")
    assert not is_ignored("docs/.tools/guide.md")
    assert not hostile_root.exists()


@pytest.fixture
def is_ignored(tmp_path):
    return make_policy_checker(tmp_path / "policy")


def test_external_global_excludes_do_not_change_project_policy(tmp_path, monkeypatch):
    excludes_path = tmp_path / "global-excludes"
    excludes_path.write_text("config/settings.json\n", encoding="utf-8")
    config_path = tmp_path / "global.gitconfig"
    config_path.write_text(
        "[core]\n\texcludesFile = {}\n".format(excludes_path), encoding="utf-8"
    )
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(config_path))
    is_ignored = make_policy_checker(tmp_path / "policy")

    assert not is_ignored("config/settings.json")


@pytest.mark.parametrize(
    "path",
    (
        ".worktrees/feature/HEAD",
        ".superpowers/sdd/plan.md",
        ".agents/cache/state.json",
        ".codex/session.json",
        ".tools/bin/tool",
        "workspace/build/netft_driver/generated.stamp",
        "workspace/devel/setup.bash",
        "workspace/install/setup.bash",
        "workspace/log/latest_build/events.json",
        ".catkin_tools/profiles/default/config.yaml",
        "CMakeFiles/netft_driver.dir/build.make",
        "generated/CMakeCache.txt",
        "compile_commands.json",
        "netft_driver/__pycache__/cache-entry",
        "netft_driver/client.cpython-312.pyc",
        "netft_driver.egg-info/PKG-INFO",
        "dist/netft_driver-0.1.0.tar.gz",
        ".pytest_cache/v/cache/nodeids",
        ".coverage.worker",
        "htmlcov/index.html",
        ".mypy_cache/3.12/cache",
        ".ruff_cache/content",
        ".pixi/envs/test/conda-meta/history",
        ".venv/bin/python",
        "venv/bin/python",
        "env/runtime/state.json",
        "ENV/runtime/state.json",
        "logs/runtime/state.json",
        "tmp/session.dat",
        "temp/session.dat",
        "Testing/results.xml",
        "core",
        "core.crash",
        ".env",
        ".env.local",
        ".vscode/settings.json",
        ".idea/workspace.xml",
        "notes.swp",
        ".DS_Store",
        "driver.log",
        "scratch.tmp",
        "session.bag",
        "session.bag.active",
        "rosbag2_20260720/metadata.yaml",
        "session.mcap",
        ".ros/runtime/state.json",
    ),
)
def test_generated_and_local_paths_are_ignored(path, is_ignored):
    assert is_ignored(path), path


@pytest.mark.parametrize(
    "path",
    (
        ".gitignore",
        "pixi.lock",
        "pixi.toml",
        "package.xml",
        "CMakeLists.txt",
        "README.md",
        "docs/architecture.md",
        "docs/.tools/guide.md",
        "fixtures/.worktrees/example.txt",
        "docs/.superpowers/guide.md",
        "fixtures/.agents/example.json",
        "src/.codex/config.json",
        "src/.pixi/manifest.toml",
        "docs/.venv/setup.md",
        "fixtures/venv/example.txt",
        "docs/env/guide.md",
        "fixtures/ENV/example.txt",
        "config/.ros/settings.yaml",
        "docs/logs/format.md",
        "fixtures/tmp/input.dat",
        "docs/temp/guide.md",
        "docs/Testing/guide.md",
        "src/core",
        "fixtures/core.crash",
        "launch/netft.launch",
        "launch/netft.launch.py",
        "config/netft_ros1.yaml",
        "config/netft_ros2.yaml",
        "netft_driver/client.py",
        "test/test_client.py",
        ".env.example",
        ".env.sample",
        "fixtures/example.db3",
        "fixtures/recording.yaml",
        "fixtures/result.xml",
        "config/settings.json",
        "src/driver.cpp",
        "src/Testing/test_case.cpp",
        "include/netft_driver/client.hpp",
        "src/Makefile",
        "config/colcon.meta",
    ),
)
def test_project_inputs_are_not_ignored(path, is_ignored):
    assert not is_ignored(path), path
