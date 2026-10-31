#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check telemetry URL faults with the owner's unchanged literal Jest oracles."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
TARGET = "web/src/telemetryUrl.ts"
FAULTS = (
    ("implicit-endpoint", '    : "";', '    : "http://localhost/synthetic-console";'),
    (
        "unchecked-scheme",
        '(url.protocol !== "http:" && url.protocol !== "https:")',
        "false",
    ),
    (
        "embedded-credentials",
        " ||\n      url.username ||\n      url.password",
        "",
    ),
    ("lost-query", "return url.href;", 'url.search = ""; return url.href;'),
    (
        "device-side-added",
        "return url.href;",
        'url.searchParams.set("side", "right"); return url.href;',
    ),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    files = (
        subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
        .decode()
        .split("\0")
    )
    files = [name for name in files if name]
    before = {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in files
    }
    original = (ROOT / TARGET).read_text()
    results = []
    try:
        for name, pattern, replacement in (
            ("baseline-before", None, None),
            *FAULTS,
            ("baseline-after", None, None),
        ):
            case = output / name
            source = case / "source"
            source.mkdir(parents=True)
            for relative in files:
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            (source / "web/node_modules").symlink_to(
                ROOT / "web/node_modules", target_is_directory=True
            )
            if pattern is not None:
                if original.count(pattern) != 1:
                    raise RuntimeError(
                        f"Mutation selector must match exactly once: {name}"
                    )
                (source / TARGET).write_text(original.replace(pattern, replacement))
            report = case / "jest.json"
            command = [
                "bash",
                "tests/run-web.sh",
                "--no-cache",
                "--runTestsByPath",
                "../tests/web/telemetryUrl.spec.ts",
                "--json",
                "--outputFile",
                str(report),
            ]
            (case / "command.json").write_text(json.dumps(command, indent=2) + "\n")
            started = time.time()
            with (case / "raw.log").open("xb") as log:
                process = subprocess.run(
                    command,
                    cwd=source,
                    env=dict(os.environ),
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=120,
                )
            evidence = json.loads(report.read_text()) if report.exists() else {}
            failed = evidence.get("numFailedTests", 0)
            runtime_errors = evidence.get("numRuntimeErrorTestSuites", 0)
            if pattern is None:
                passed = process.returncode == 0 and evidence.get("success") is True
            else:
                passed = process.returncode == 1 and failed > 0 and runtime_errors == 0
            results.append(
                dict(
                    name=name,
                    command=command,
                    start=started,
                    end=time.time(),
                    exit=process.returncode,
                    failed_assertions=failed,
                    runtime_error_suites=runtime_errors,
                    status="PASS" if passed else "FAIL",
                )
            )
            (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            if not passed:
                raise RuntimeError(f"Unqualified telemetry URL result: {name}")
            print(f"PASS {name}", flush=True)
    finally:
        after = {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in files
        }
        (output / "source-before.json").write_text(json.dumps(before, indent=2) + "\n")
        (output / "source-after.json").write_text(json.dumps(after, indent=2) + "\n")
        if before != after:
            raise RuntimeError("Owner source changed during telemetry URL controls")


if __name__ == "__main__":
    main()
