#!/usr/bin/env python3
"""Real-kernel common metadata and assertion-forwarding contract, identical repeats."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--zephyr", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
p.add_argument("--jobs", type=int, default=2)
a = p.parse_args()
if not a.output.is_absolute() or a.output.exists() or a.jobs < 1:
    p.error("choose a fresh absolute output and positive job budget")
a.output.mkdir(parents=True)
root = Path(__file__).resolve().parents[2]
env = dict(os.environ, ZEPHYR_BASE=str(a.zephyr), PYTHONDONTWRITEBYTECODE="1")
rows = []
control = a.output / "unpublished-operation.c"
text = (root / "src/watchdog_context.c").read_text()
selector = "atomic_set(&generation, (atomic_val_t)(gen + 1U));"
assert text.count(selector) == 1
control.write_text(
    text.replace(selector, "(void)gen; /* controlled missing publication */")
)
clock_control = a.output / "assert-clock-operation.c"
selector = "atomic_set(&assertion, 2);"
assert text.count(selector) == 1
clock_control.write_text(
    text.replace(
        selector, selector + "\n    zmk_watchdog_operation_at(0, 1, line, 0, site);"
    )
)
for name, extra in [
    ("file", []),
    ("no-file", ["-DCONFIG_ASSERT_NO_FILE_INFO=y"]),
    ("unpublished-control", ["-DWATCHDOG_CONTEXT_UNIT=" + str(control)]),
    ("assert-clock-control", ["-DWATCHDOG_CONTEXT_UNIT=" + str(clock_control)]),
]:
    out = a.output / name
    out.mkdir()
    cmd = [
        "west",
        "build",
        "-o=-j" + str(a.jobs),
        "-b",
        "native_sim/native/64",
        "-s",
        str(root / "tests/context"),
        "-d",
        str(out / "build"),
        "--",
        "-DWEST=OFF",
        "-DZEPHYR_MODULES=",
        *extra,
    ]
    (out / "command.json").write_text(json.dumps(cmd, indent=2))
    with (out / "build.log").open("w") as log:
        r = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        raise SystemExit((out / "build.log").read_text()[-6000:])
    binary = out / "build/zephyr/zephyr.exe"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    for repeat in range(1, 4):
        started = time.time_ns()
        try:
            r = subprocess.run([str(binary)], capture_output=True, timeout=20)
        except subprocess.TimeoutExpired as error:
            (out / f"run-{repeat}.stdout").write_bytes(error.stdout or b"")
            (out / f"run-{repeat}.stderr").write_bytes(error.stderr or b"")
            (out / f"run-{repeat}.timeout").write_text(
                "20 seconds; incomplete process exit"
            )
            raise
        (out / f"run-{repeat}.stdout").write_bytes(r.stdout)
        (out / f"run-{repeat}.stderr").write_bytes(r.stderr)
        rows.append(
            dict(
                variant=name,
                repeat=repeat,
                exit=r.returncode,
                start_unix_ns=started,
                elf_sha256=digest,
            )
        )
        (a.output / "runs.json").write_text(json.dumps(rows, indent=2))
        if name in ("unpublished-control", "assert-clock-control"):
            assert (
                r.returncode == 99
                and b"WATCHDOG_CONTEXT_ASSERT_FAILURE" in r.stdout
                and b"ASSERTION FAIL" in r.stdout
            ), (name, repeat, r.stdout, r.stderr)
        else:
            assert r.returncode == 0 and b"WATCHDOG_CONTEXT_COMPLETE" in r.stdout, (
                name,
                repeat,
                r.stdout,
                r.stderr,
            )
print("WATCHDOG_CONTEXT_PASS variants=2 repeats=3 control_rejected=6")
