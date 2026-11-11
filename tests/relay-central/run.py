#!/usr/bin/env python3
"""Central dispatch with controlled broadcast transport and peer capacities."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True)
    owner = Path(__file__).resolve().parents[2]
    projects = {}
    for name in ["zephyr", "cmsis", "nanopb", "zmk"]:
        projects[name] = Path(
            subprocess.check_output(
                ["west", "list", name, "-f", "{abspath}"],
                cwd=args.workspace,
                text=True,
            ).strip()
        ).resolve()
    env = dict(os.environ, ZEPHYR_BASE=str(projects["zephyr"]))
    identity = {}
    for name, path in {"owner": owner, **projects}.items():
        result = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
        )
        identity[name] = dict(revision=result.stdout.strip(), exit=result.returncode)
    identity["fixture_inputs"] = {
        str(path.relative_to(owner)): hashlib.sha256(path.read_bytes()).hexdigest()
        for directory in ["src/split", "include", "proto", "tests/relay-central"]
        for path in (owner / directory).rglob("*")
        if path.is_file() and "__pycache__" not in path.parts
    }
    (output / "inputs.json").write_text(json.dumps(identity, indent=2))
    rows = []
    for count in [1, 2]:
        build = output / str(count)
        command = [
            "west",
            "build",
            "-b",
            "native_sim/native/64",
            "-s",
            str(owner / "tests/relay-central"),
            "-d",
            str(build),
            "--",
            "-DWEST=OFF",
            "-DZMK_ROOT=" + str(projects["zmk"]),
            "-DPEER_COUNT=" + str(count),
            "-DZEPHYR_MODULES="
            + ";".join(str(projects[n]) for n in ["cmsis", "nanopb"]),
        ]
        with (output / f"build-{count}.log").open("w") as log:
            result = subprocess.run(
                command, env=env, stdout=log, stderr=subprocess.STDOUT
            )
        if result.returncode:
            raise SystemExit(result.returncode)
        binary = build / "zephyr/zephyr.exe"
        for trial in range(1, 4):
            result = subprocess.run(
                [str(binary)], env=env, capture_output=True, text=True, timeout=15
            )
            text = result.stdout + result.stderr
            (output / f"run-{count}-{trial}.log").write_text(text)
            passed = (
                result.returncode == 0
                and "RESULT: PASS failures=0" in text
                and "FAIL line=" not in text
                and (
                    count != 1
                    or "CENTRAL_NOTIFICATION PASS groups=6 notifications=5 queued=20 capacity=21"
                    in text
                )
            )
            rows.append(
                dict(
                    capacity=count,
                    trial=trial,
                    exit=result.returncode,
                    passed=passed,
                    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    build_command=command,
                )
            )
            (output / "results.json").write_text(json.dumps(rows, indent=2))
            print(
                f"Central capacity={count} trial={trial}: {'PASS' if passed else 'FAIL'}",
                flush=True,
            )
    raise SystemExit(int(any(not row["passed"] for row in rows)))


if __name__ == "__main__":
    main()
