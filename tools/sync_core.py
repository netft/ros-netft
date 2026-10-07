#!/usr/bin/env python3
"""Synchronize an exact upstream checkout and verify the selected snapshot bytes.

The digest detects content drift. Git identity is checked at sync time; a digest
alone is not a signature or proof that a remote release was published.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

REPOSITORY = "https://github.com/netft/netft-cpp.git"
COMMIT = "91f012c5d6f9b63902765ccbec3437cb286c15e1"
SELECTED = ('LICENSE', 'include/netft', 'src')
ROOT = Path(__file__).resolve().parents[1] / "src/core"


def git(source: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def files(root: Path) -> list[Path]:
    result = [root / "UPSTREAM"]
    for name in SELECTED:
        path = root / name
        if not path.exists():
            raise SystemExit(f"missing snapshot path: {name}")
        result.extend(path.rglob("*") if path.is_dir() else [path])
    if any(path.is_symlink() for path in result):
        raise SystemExit("snapshot must not contain symbolic links")
    return sorted(path for path in result if path.is_file())


def manifest(root: Path) -> str:
    return "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}\n"
        for path in files(root)
    )


def verify(root: Path = ROOT) -> None:
    metadata = dict(line.split("=", 1) for line in (root / "UPSTREAM").read_text().splitlines())
    if (metadata.get("repository") != REPOSITORY or metadata.get("commit") != COMMIT
            or metadata.get("tag") != "unreleased" or metadata.get("paths") != ",".join(SELECTED)):
        raise SystemExit("snapshot identity mismatch")
    if (root / "SNAPSHOT.sha256").read_text() != manifest(root):
        raise SystemExit("snapshot checksum mismatch")


def sync(source: Path, root: Path = ROOT, commit: str = COMMIT) -> None:
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or commit != COMMIT:
        raise SystemExit("unsupported upstream commit")
    if git(source, "remote", "get-url", "origin") != REPOSITORY:
        raise SystemExit("source repository mismatch")
    if git(source, "rev-parse", "HEAD") != commit or git(source, "status", "--porcelain"):
        raise SystemExit("source must be clean at the requested commit")
    # Stage first, preserving the consumer's private build file. Swap only after
    # every selected path and the generated manifest have been verified.
    root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".core-sync-", dir=root.parent) as temporary:
        transaction = Path(temporary)
        staging = transaction / "snapshot"
        previous = transaction / "previous"
        if root.exists():
            shutil.copytree(root, staging)
        else:
            staging.mkdir()
        for name in SELECTED:
            origin, target = source / name, staging / name
            if origin.is_symlink() or (origin.is_dir() and any(p.is_symlink() for p in origin.rglob("*"))):
                raise SystemExit("source must not contain symbolic links")
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
            target.parent.mkdir(parents=True, exist_ok=True)
            if origin.is_dir():
                shutil.copytree(origin, target)
            else:
                shutil.copy2(origin, target)
        (staging / "UPSTREAM").write_text(
            f"repository={REPOSITORY}\ntag=unreleased\ncommit={commit}\nlicense=Apache-2.0\npaths={','.join(SELECTED)}\nadaptations=none\n"
        )
        (staging / "SNAPSHOT.sha256").write_text(manifest(staging))
        verify(staging)
        if root.exists():
            root.rename(previous)
        try:
            staging.rename(root)
        except BaseException:
            if previous.exists():
                previous.rename(root)
            raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    update = commands.add_parser("sync")
    update.add_argument("--source", required=True, type=Path)
    update.add_argument("--commit", required=True)
    commands.add_parser("verify")
    arguments = parser.parse_args()
    if arguments.command == "sync":
        sync(arguments.source.resolve(), commit=arguments.commit)
    else:
        verify()


if __name__ == "__main__":
    main()
