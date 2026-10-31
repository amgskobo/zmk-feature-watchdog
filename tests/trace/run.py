#!/usr/bin/env python3
"""Bounded Cortex-M4 capture on real Zephyr/QEMU; not nRF board acceptance."""

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support.qemu_capture import complete_terminal_record, wait_for_terminal_record  # noqa: E402


def validate_trace(text, elf, a2l, assert_case=False):
    assert complete_terminal_record(text), "incomplete terminal record"
    assert "RESULT: PASS failures=0" in text, "kernel capture failed"
    traces = [
        json.loads(line[6:]) for line in text.splitlines() if line.startswith("TRACE ")
    ]
    assert [t["name"] for t in traces] == ["pended", "asleep", "running"]
    named = {}
    for trace in traces:
        addresses = [
            a if i == 0 else (a & ~1) - 2 for i, a in enumerate(trace["addresses"])
        ]
        response = subprocess.run(
            [a2l, "-f", "-C", "-i", "-p", "-e", str(elf)] + [hex(a) for a in addresses],
            capture_output=True,
            text=True,
            check=True,
        )
        named[trace["name"]] = response.stdout
    assert "wait_for_mutex" in named["pended"] and "waiter_entry" in named["pended"]
    assert "spin at" in named["running"] and "spinner_entry" in named["running"]
    fatal = [
        json.loads(line[6:]) for line in text.splitlines() if line.startswith("FATAL ")
    ][0]
    assert fatal["build"] == 0x12345678 and fatal["xpsr"] & 511
    code = [hex(fatal["pc"] & ~1), hex((fatal["lr"] & ~1) - 2)]
    if assert_case:
        assert fatal["origin"] > 2
        code.append(hex((fatal["origin"] & ~1) - 2))
    response = subprocess.run(
        [
            a2l,
            "-f",
            "-C",
            "-i",
            "-e",
            str(elf),
        ]
        + code,
        capture_output=True,
        text=True,
        check=True,
    )
    assert "fatal_timer_handler" in response.stdout
    named["fatal"] = response.stdout
    return named


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--assert-context", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.artifacts.resolve()
    out.mkdir(parents=True, exist_ok=False)
    workspace = args.workspace.resolve()
    projects = {
        name: Path(
            subprocess.check_output(
                ["west", "list", name, "-f", "{abspath}"], cwd=workspace, text=True
            ).strip()
        )
        for name in ("zephyr", "cmsis", "cmsis_6")
    }
    env = dict(
        os.environ, ZEPHYR_BASE=str(projects["zephyr"]), PYTHONDONTWRITEBYTECODE="1"
    )
    modules = ";".join(str(projects[name]) for name in ("cmsis", "cmsis_6"))
    inputs = {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for folder in ("src", "include", "tests/trace")
        for path in (root / folder).rglob("*")
        if path.is_file() and "__pycache__" not in path.parts
    }
    inputs["tests/support/qemu_capture.py"] = hashlib.sha256(
        (root / "tests/support/qemu_capture.py").read_bytes()
    ).hexdigest()
    (out / "inputs.json").write_text(
        json.dumps(
            {
                "owner_inputs": inputs,
                "projects": {
                    name: {
                        "path": str(path),
                        "revision": subprocess.check_output(
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
                        ).strip(),
                    }
                    for name, path in projects.items()
                },
            },
            indent=2,
        )
    )
    command = [
        "west",
        "build",
        "-b",
        "mps2/an386",
        "-s",
        str(root / "tests/trace"),
        "-d",
        str(out / "build"),
        "--",
        "-DZEPHYR_MODULES=" + modules,
    ]
    if args.assert_context:
        command.append("-DWATCHDOG_ASSERT_CASE=ON")
    (out / "command.json").write_text(json.dumps(command, indent=2))
    with (out / "build.log").open("w") as log:
        result = subprocess.run(
            command, cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT
        )
    if result.returncode:
        print((out / "build.log").read_text()[-12000:])
        raise SystemExit(result.returncode)
    elf = out / "build/zephyr/zephyr.elf"
    qemu = sorted(glob.glob("/opt/**/qemu-system-arm", recursive=True))[0]
    a2l = sorted(glob.glob("/opt/**/arm-zephyr-eabi-addr2line", recursive=True))[0]
    trials = []
    for trial in range(1, 4):
        raw = out / f"run-{trial}.log"
        command = [
            qemu,
            "-cpu",
            "cortex-m4",
            "-machine",
            "mps2-an386",
            "-display",
            "none",
            "-monitor",
            "none",
            "-vga",
            "none",
            "-net",
            "none",
            "-icount",
            "shift=6,align=off,sleep=off",
            "-rtc",
            "clock=vm",
            "-serial",
            "file:" + str(raw),
            "-kernel",
            str(elf),
        ]
        with (out / f"qemu-{trial}.log").open("w") as log:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=log, stderr=log
            )
            try:
                wait_for_terminal_record(
                    process,
                    lambda: raw.read_text(errors="replace") if raw.exists() else "",
                )
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
        text = raw.read_text().replace("\r", "")
        print(text)
        failure = None
        try:
            named = validate_trace(text, elf, a2l, args.assert_context)
            (out / f"decoded-{trial}.json").write_text(json.dumps(named, indent=2))
        except Exception:
            failure = traceback.format_exc()
            (out / f"validation-{trial}.log").write_text(failure)
        trials.append(
            dict(
                trial=trial,
                status="FAIL" if failure else "PASS",
                command=command,
                process_exit=process.returncode,
                elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
                terminal_complete=complete_terminal_record(text),
                validation_error=failure,
            )
        )
        (out / "results.json").write_text(json.dumps(trials, indent=2))
        print(f"Watchdog trace trial={trial} status={trials[-1]['status']}")
    raise SystemExit(any(row["status"] != "PASS" for row in trials))


if __name__ == "__main__":
    main()
