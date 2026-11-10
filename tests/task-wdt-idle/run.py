#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real software-monitor timer, strict sanitizers and specified original controls."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def source_identity(workspace):
    paths = [
        ROOT / "Kconfig",
        ROOT / "CMakeLists.txt",
        workspace / "zmk/app/src/workqueue.c",
        workspace / "zephyr/subsys/task_wdt/task_wdt.c",
    ]
    for directory in ["src", "include", "tests/task-wdt-idle"]:
        paths.extend(p for p in (ROOT / directory).rglob("*") if p.is_file())
    dependencies = {}
    for name in ["zmk", "zephyr"]:
        path = workspace / name
        command = ["git", "-c", "safe.directory=" + str(path), "-C", str(path)]
        assert not subprocess.check_output(
            [*command, "status", "--porcelain"], text=True
        ), name
        dependencies[name] = subprocess.check_output(
            [*command, "rev-parse", "HEAD"], text=True
        ).strip()
    return dict(
        dependencies=dependencies,
        hashes={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if args.output.is_symlink() or output.exists():
        parser.error("Use a fresh output directory")
    output.mkdir(parents=True)
    workspace = args.workspace.resolve()
    before = source_identity(workspace)
    (output / "source-before.json").write_text(json.dumps(before, indent=2))
    env = dict(
        os.environ,
        ZEPHYR_BASE=str(workspace / "zephyr"),
        Zephyr_DIR=str(workspace / "zephyr/share/zephyr-package/cmake"),
        PYTHONDONTWRITEBYTECODE="1",
    )
    cases = [
        ("sys-only", False, 10000),
        ("both-queues", True, 10000),
        ("old-100ms", False, 100),
        ("unsafe-fallback-off", False, None),
    ]
    records = []
    diagnostic = re.compile(r"(?:Sanitizer:|[^\n]+:\d+(?::\d+)?: runtime error:)")
    for name, lowprio, minimum in cases:
        case = output / name
        case.mkdir()
        conf = case / "selection.conf"
        conf.write_text(
            "CONFIG_TASK_WDT_HW_FALLBACK="
            + ("y" if minimum else "n")
            + "\n"
            + (f"CONFIG_TASK_WDT_MIN_TIMEOUT={minimum}\n" if minimum else "")
        )
        command = [
            "west",
            "build",
            "-p=always",
            "-b",
            "native_sim/native/64",
            "-s",
            str(ROOT / "tests/task-wdt-idle"),
            "-d",
            str(case / "build"),
            "--",
            "-DWEST=OFF",
            "-DZEPHYR_MODULES=",
            "-DZMK_ROOT=" + str(workspace / "zmk"),
            "-DEXTRA_CONF_FILE=" + str(conf),
            "-DWATCHDOG_IDLE_LOWPRIO=" + ("ON" if lowprio else "OFF"),
            "-DEXTRA_CFLAGS=-fno-sanitize-recover=all",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        row = dict(name=name, command=command, start=time.time())
        records.append(row)
        (output / "results.json").write_text(json.dumps(records, indent=2))
        with (case / "build.log").open("x") as log:
            result = subprocess.run(
                command,
                cwd=ROOT,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=900,
            )
        row["build_exit"] = result.returncode
        (output / "results.json").write_text(json.dumps(records, indent=2))
        if result.returncode:
            raise RuntimeError("Build failed; not a qualified runtime control: " + name)
        binary = case / "build/zephyr/zephyr.exe"
        row["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        config = (case / "build/zephyr/.config").read_text().splitlines()
        for flag in [
            "CONFIG_ASAN=y",
            "CONFIG_UBSAN=y",
            "# CONFIG_ASAN_RECOVER is not set",
            "CONFIG_TIMEOUT_64BIT=y",
        ]:
            assert flag in config, (name, flag)
        commands = json.loads((case / "build/compile_commands.json").read_text())
        core = next(c["command"] for c in commands if c["file"].endswith("/task_wdt.c"))
        assert "-fsanitize=address,undefined" in core
        assert (
            "-fno-sanitize-recover=all" in core and "-fsanitize-recover=all" not in core
        )
        row["runs"] = []
        for repeat in range(1, 4):
            started = time.time()
            result = subprocess.run(
                [str(binary), "-no-rt"],
                cwd=case,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=20,
            )
            (case / f"run-{repeat}.log").write_text(result.stdout)
            row["runs"].append(
                dict(exit=result.returncode, start=started, end=time.time())
            )
            (output / "results.json").write_text(json.dumps(records, indent=2))
            if name == "unsafe-fallback-off":
                assert result.returncode == 1 and re.search(
                    r"task_wdt\.c:79(?::\d+)?: runtime error: signed integer overflow:",
                    result.stdout,
                )
            elif name == "old-100ms":
                assert (
                    result.returncode == 1
                    and "FAIL watchdog idle background timer" in result.stdout
                )
                assert not diagnostic.search(result.stdout)
            else:
                assert (
                    result.returncode == 0
                    and result.stdout.count("PASS watchdog idle timer") == 1
                )
                assert "FAIL " not in result.stdout and not diagnostic.search(
                    result.stdout
                )
            assert (
                hashlib.sha256(binary.read_bytes()).hexdigest() == row["binary_sha256"]
            )
        row["end"] = time.time()
        (output / "results.json").write_text(json.dumps(records, indent=2))
    after = source_identity(workspace)
    (output / "source-after.json").write_text(json.dumps(after, indent=2))
    assert before == after, "Source or dependency drift invalidates timer results"
    print(
        "PASS: two idle timer variants x3; old-100ms and unsafe-fallback-off controls x3"
    )


if __name__ == "__main__":
    main()
