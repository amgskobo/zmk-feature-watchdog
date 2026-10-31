#!/usr/bin/env python3
"""Create a fresh isolated owner test workspace from verified local cached inputs; never fetch."""

import argparse
import json
import shutil
import hashlib
from pathlib import Path
import subprocess
import tarfile
import yaml

if not __debug__:
    raise RuntimeError("Workspace identity assertions must remain enabled")

p = argparse.ArgumentParser(description=__doc__)
p.add_argument(
    "--owner",
    type=Path,
    help="Copy exact owner inputs inside workspace for tools requiring cwd containment",
)
p.add_argument("--cache", type=Path, required=True)
p.add_argument("--commands", type=Path, required=True)
p.add_argument("--settings-tar", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)


def head(path):
    return subprocess.check_output(
        [
            "git",
            "-c",
            "safe.directory=" + str(path),
            "-C",
            str(path),
            "rev-parse",
            "HEAD",
        ],
        text=True,
    ).strip()


assert head(a.cache / "zmk") == "e5c9b6915b56801193e359dd9bad4a167ce0d1b8"
assert head(a.cache / "zephyr") == "10ba6d0cb38bc3d258775d27982f707599320085"
assert head(a.commands) == "536260056f362edb2092905f2a75317bc8cb9bc2"
resolved = yaml.safe_load(
    subprocess.check_output(["west", "manifest", "--resolve"], cwd=a.cache)
)
projects = []
identities = []
for entry in resolved["manifest"]["projects"]:
    rel = entry.get("path", entry["name"])
    if entry["name"] not in ["zmk", "zephyr"] and not rel.startswith(
        ("modules/", "optional/")
    ):
        continue
    source = a.cache / rel
    if not source.is_dir():
        raise RuntimeError("Required checkout inaccessible: " + str(source))
    destination = a.output / rel
    destination.parent.mkdir(parents=True, exist_ok=True)
    if entry["name"] in ["zmk", "zephyr"]:
        # CMake canonicalizes these roots. A symlink would escape the isolated
        # workspace and silently discover the production module manifest.
        shutil.copytree(
            source, destination, ignore=shutil.ignore_patterns(".git", "__pycache__")
        )
        if (source / ".git").exists():
            (destination / ".git").symlink_to(
                source / ".git", target_is_directory=(source / ".git").is_dir()
            )
    else:
        destination.symlink_to(source, target_is_directory=True)
    project = {
        key: entry[key]
        for key in ["name", "revision", "url", "path", "west-commands"]
        if key in entry
    }
    projects.append(project)
    identities.append(
        {"name": entry["name"], "path": str(source), "revision": head(source)}
    )
(a.output / "zmk-west-commands").symlink_to(a.commands, target_is_directory=True)
projects.append(
    {
        "name": "zmk-west-commands",
        "path": "zmk-west-commands",
        "revision": head(a.commands),
        "url": "https://github.com/cormoran/zmk-west-commands",
        "west-commands": "scripts/west-commands.yml",
    }
)
settings = a.output / "zmk-feature-custom-settings"
settings.mkdir()
with tarfile.open(a.settings_tar) as archive:
    archive.extractall(settings, filter="data")
projects.append(
    {
        "name": "zmk-feature-custom-settings",
        "path": "zmk-feature-custom-settings",
        "revision": "c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f",
        "url": "https://github.com/cormoran/zmk-feature-custom-settings",
    }
)
manifest = a.output / "manifest"
manifest.mkdir()
(manifest / "west.yml").write_text(
    yaml.safe_dump({"manifest": {"projects": projects, "self": {"path": "manifest"}}})
)
west = a.output / ".west"
west.mkdir()
(west / "config").write_text("[manifest]\npath = manifest\nfile = west.yml\n")
(a.output / "zephyr.identity.json").write_text(json.dumps(identities, indent=2))
print(a.output)

if a.owner:
    target = a.output / "owner"
    shutil.copytree(
        a.owner,
        target,
        ignore=shutil.ignore_patterns(
            ".git",
            "node_modules",
            "__pycache__",
            "dist",
            ".pytest_cache",
            "*.tsbuildinfo",
        ),
    )
    identities = {}
    for path in target.rglob("*"):
        if path.is_file():
            rel = path.relative_to(target).as_posix()
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            assert digest == hashlib.sha256((a.owner / rel).read_bytes()).hexdigest()
            identities[rel] = digest
    (a.output / "owner-copy-identity.json").write_text(json.dumps(identities, indent=2))
