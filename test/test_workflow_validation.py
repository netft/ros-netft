import shlex
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def workflow_run_blocks(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    blocks = []
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if not stripped.startswith("run:"):
            continue
        indent = len(line) - len(stripped)
        scalar = stripped.removeprefix("run:").strip()
        if scalar not in {"|", ">-"}:
            blocks.append(scalar)
            continue
        body = []
        for following in lines[index + 1 :]:
            following_stripped = following.lstrip()
            following_indent = len(following) - len(following_stripped)
            if following_stripped and following_indent <= indent:
                break
            body.append(following_stripped)
        blocks.append(" ".join(body))
    return blocks


def installed_apt_packages(command):
    tokens = shlex.split(command)
    try:
        install = tokens.index("install")
    except ValueError:
        return set()
    return {
        token
        for token in tokens[install + 1 :]
        if not token.startswith("-") and token not in {"sudo", "apt-get"}
    }


def test_core_coverage_installs_every_native_build_dependency():
    commands = workflow_run_blocks(ROOT / ".github/workflows/coverage.yml")
    packages = set().union(*(installed_apt_packages(command) for command in commands))
    assert {"gcovr", "libgtest-dev", "libcurl4-openssl-dev"} <= packages
