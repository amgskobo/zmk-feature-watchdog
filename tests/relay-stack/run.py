#!/usr/bin/env python3
"""Real Cortex-M/2048-byte relay stack; controlled store and event transport."""

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support.qemu_capture import complete_terminal_record, wait_for_terminal_record  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("zephyr", "cmsis", "nanopb", "zmk", "artifacts"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument(
        "--fairness",
        action="store_true",
        help="Run the production freeze monitor against a continuously replenished relay FIFO",
    )
    parser.add_argument(
        "--mute-info-negative-control",
        action="store_true",
        help="Mute production INFO; the log contract must fail, without source rewriting",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.artifacts.resolve()
    output.mkdir(parents=True, exist_ok=False)
    projects = {
        name: getattr(args, name).resolve()
        for name in ("zephyr", "cmsis", "nanopb", "zmk")
    }
    identity = {}
    for name, path in {"owner": root, **projects}.items():
        result = subprocess.run(
            [
                "git",
                "-c",
                "safe.directory=" + str(path),
                "-C",
                str(path),
                "rev-parse",
                "HEAD",
            ],
            capture_output=True,
            text=True,
        )
        identity[name] = dict(
            path=str(path), revision=result.stdout.strip(), exit=result.returncode
        )
    identity["owner_inputs"] = {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for folder in ("src", "include", "proto", "tests/relay-stack")
        for p in (root / folder).rglob("*")
        if p.is_file() and "__pycache__" not in p.parts
    }
    identity["owner_inputs"]["tests/support/qemu_capture.py"] = hashlib.sha256(
        (root / "tests/support/qemu_capture.py").read_bytes()
    ).hexdigest()
    (output / "inputs.json").write_text(json.dumps(identity, indent=2))
    env = dict(
        os.environ, ZEPHYR_BASE=str(projects["zephyr"]), PYTHONDONTWRITEBYTECODE="1"
    )
    build = Path(tempfile.mkdtemp(prefix="watchdog-relay-stack-"))
    command = [
        "west",
        "build",
        "-b",
        "mps2/an386",
        "-s",
        str(root / "tests/relay-stack"),
        "-d",
        str(build),
        "--",
        "-DWEST=OFF",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DZMK_ROOT=" + str(projects["zmk"]),
        "-DZEPHYR_MODULES="
        + ";".join(str(projects[name]) for name in ("cmsis", "nanopb")),
    ]
    if args.mute_info_negative_control:
        command.append("-DWATCHDOG_LOG_LEVEL=0")
    if args.fairness:
        command.extend(
            [
                "-DWATCHDOG_RELAY_FAIRNESS=ON",
                "-DEXTRA_CONF_FILE=" + str(root / "tests/relay-stack/fairness.conf"),
            ]
        )
    (output / "command.json").write_text(json.dumps(command, indent=2))
    with (output / "build.log").open("w") as stream:
        result = subprocess.run(
            command, env=env, stdout=stream, stderr=subprocess.STDOUT
        )
    if result.returncode:
        print((output / "build.log").read_text()[-10000:])
        raise SystemExit(result.returncode)
    # Compile on local Linux storage; retain only reviewable inputs and results.
    # Repeats execute one identical ELF, never rebuild or overwrite old evidence.
    for name in (
        "zephyr/zephyr.elf",
        "zephyr/.config",
        "zephyr/zephyr.dts",
        "compile_commands.json",
        "proto/cormoran/watchdog/watchdog.pb.h",
        "proto/cormoran/watchdog/watchdog.pb.c",
    ):
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(build / name, destination)
    elf = output / "zephyr/zephyr.elf"
    qemu = sorted(glob.glob("/opt/**/qemu-system-arm", recursive=True))[0]
    trials = []
    for trial in range(1, 4):
        raw = output / f"run-{trial}.log"
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
        with (output / f"qemu-{trial}.log").open("w") as stream:
            process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream
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
        text = raw.read_text(errors="replace") if raw.exists() else ""
        passed = complete_terminal_record(text) and "RESULT: PASS failures=0" in text
        checks = [
            text.count("CASE seq=") == 7,
            text.count("REQUESTCASE ") == 5,
            "REQUESTCASE queued-two ordered_ids=110,111" in text,
            "REQUESTCASE running-three ordered_ids=201,202,203" in text,
            "REQUESTCASE full correlated_busy=404 recovered=405" in text,
            "REQUESTCASE malformed-page-tail page=501 packets=9 status=502" in text,
            "REQUESTCASE persisted-read-errors explicit=3 no_partial_page=1 status_tail=3"
            in text,
            text.count("REPORTCASE ") == 3,
            "CONTEXT_RELAY_COMPLETE records=4 fragments=12 packets=21" in text,
            "PASS: watchdog_common_context_relay_controls=8" in text,
            "RETIRED_HEALTHCASE unsupported" in text,
            "settings not ready after 30 attempts" in text,
            "history stored=0 capacity=16" in text,
            "history index=3 err=-5" in text,
            "fatal source=2 reason=2 pc=00001234 lr=00005678" in text,
            "diag source=3 available=0" in text,
            "diag source=1 " not in text,
            text.count("event source=0 ") == 7,
            text.count("event source=2 ") == 7,
            "abababab" not in text and "SSSS" not in text,
            "69726576" not in text and "20686375" not in text,
            "history complete attempted=6" in text,
        ]
        if args.fairness:
            checks = [
                text.count("FAIRNESSCASE ") == 2,
                "FAIRNESSCASE replenished responses=" in text,
                " freezes=0" in text,
                "FAIRNESSCASE real-stall detected=1 recovered_feed=1" in text,
            ]
        passed = passed and all(checks)
        trials.append(
            dict(
                trial=trial,
                status="PASS" if passed else "FAIL",
                process_exit=process.returncode,
                command=command,
                checks=checks,
                negative_control=args.mute_info_negative_control,
                fairness=args.fairness,
                elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
            )
        )
        print(f"Trial {trial}: {'PASS' if passed else 'FAIL'}; raw={raw}", flush=True)
        for line in text.splitlines():
            if line.startswith(
                ("CASE ", "REPORTCASE ", "FAIRNESSCASE ", "FAIL ", "RESULT:")
            ):
                print(line, flush=True)
    (output / "results.json").write_text(json.dumps(trials, indent=2))
    raise SystemExit(any(row["status"] != "PASS" for row in trials))


if __name__ == "__main__":
    main()
