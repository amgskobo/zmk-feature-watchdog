#!/usr/bin/env python3
"""Ten preserved overwrite controls with strict selectors and raw run evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MUTATIONS = [
    (
        "victim-is-newest",
        r"slots\[oldest\]\.boot_ordinal\)\s*<\s*0",
        "slots[oldest].boot_ordinal) > 0",
    ),
    (
        "victim-is-first-used",
        r"oldest < 0 \|\|\s*\(int32_t\)\(slots\[i\]\.boot_ordinal\s*-\s*slots\[oldest\]\.boot_ordinal\)\s*<\s*0",
        "oldest < 0",
    ),
    (
        "ordinal-not-wrap-safe",
        r"\(int32_t\)\(slots\[i\]\.boot_ordinal\s*-\s*slots\[oldest\]\.boot_ordinal\)\s*<\s*0",
        "slots[i].boot_ordinal < slots[oldest].boot_ordinal",
    ),
    ("guard-removed", r"rec->uptime_s >= min_uptime_s", "1"),
    (
        "guard-inverted",
        r"rec->uptime_s >= min_uptime_s",
        "rec->uptime_s < min_uptime_s",
    ),
    ("no-overwrite", r"slot = find_oldest_slot_locked\(\);", "slot = -1;"),
    (
        "stopped-in-status",
        r"out->recording_stopped = false;",
        "out->recording_stopped = out->stored >= out->capacity;",
    ),
    (
        "stopped-in-accessor",
        r"(bool zmk_watchdog_store_recording_stopped\(void\) \{\s*#if defined\(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST\)\s*)return false;",
        r"\1return zmk_watchdog_store_count() >= CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS;",
    ),
    (
        "drop-counted-on-replace",
        r"slot = find_oldest_slot_locked\(\);",
        "slot = find_oldest_slot_locked(); dropped_since_boot++;",
    ),
    (
        "stale-ordinal-on-reload",
        r"meta.next_boot_ordinal = slots\[i\]\.boot_ordinal \+ 1U;",
        "meta.next_boot_ordinal = meta.next_boot_ordinal;",
    ),
]


def mutate(source, name, pattern, replacement):
    changed, count = re.subn(pattern, replacement, source)
    if count != 1 or changed == source:
        raise ValueError(f"MUTATION DID NOT APPLY EXACTLY ONCE: {name} count={count}")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile-failure-negative-control", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/watchdog_store.c").read_bytes().decode("utf-8")
    requested = os.environ.get("WATCHDOG_MUTATION_ARTIFACTS")
    if requested:
        output = Path(requested).resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="watchdog-overwrite-results-"))
    print(f"Overwrite mutation evidence: {output}", flush=True)
    inputs = {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for folder in ("src", "include", "tests/store")
        for path in (root / folder).rglob("*")
        if path.is_file() and "__pycache__" not in path.parts
    }
    (output / "inputs.json").write_text(json.dumps(inputs, indent=2))
    (output / "compiler.txt").write_text(
        subprocess.check_output(["cc", "--version"], text=True)
    )
    results = []

    def save():
        (output / "results.json").write_text(json.dumps(results, indent=2))

    with tempfile.TemporaryDirectory(prefix="watchdog-overwrite-work-") as temporary:
        work = Path(temporary)
        module = work / "module"
        shutil.copytree(root / "tests/store", module / "tests/store")
        shutil.copytree(root / "include", module / "include")
        shutil.copytree(root / "src", module / "src")

        def run(name, changed, capacity):
            case = output / f"{name}-capacity-{capacity}"
            case.mkdir()
            (module / "src/watchdog_store.c").write_text(changed)
            (case / "watchdog_store.c").write_text(changed)
            binary = work / "test"
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-O1",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                f"-DCONFIG_ZMK_WATCHDOG_MAX_INCIDENTS={capacity}",
                "-DCONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST=1",
                "-DCONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S=10",
                "-I" + str(module / "tests/store"),
                "-I" + str(module / "include"),
                str(module / "tests/store/store_test.c"),
                "-o",
                str(binary),
            ]
            row = dict(name=name, capacity=capacity, command=command)
            (case / "command.json").write_text(json.dumps(command, indent=2))
            with (case / "compile.log").open("wb") as log:
                compiled = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            row["compile_exit"] = compiled.returncode
            if compiled.returncode:
                row["status"] = "COMPILE FAILED; NOT KILLED"
                results.append(row)
                save()
                raise RuntimeError(
                    f"Compile failed: {name} capacity={capacity}; not a killed mutant"
                )
            row["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
            with (case / "raw.log").open("wb") as log:
                try:
                    result = subprocess.run(
                        [str(binary)],
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"),
                        timeout=120,
                    )
                except subprocess.TimeoutExpired:
                    row["status"] = "RUN TIMEOUT; NOT KILLED"
                    results.append(row)
                    save()
                    raise
            row["run_exit"] = result.returncode
            raw = (case / "raw.log").read_text(errors="replace")
            diagnosed = (
                ("Assertion " in raw and "failed" in raw)
                or "ERROR: AddressSanitizer:" in raw
                or "runtime error:" in raw
            )
            row["status"] = (
                "PASS"
                if result.returncode == 0
                else ("CONTRACT FAILURE" if diagnosed else "UNCLASSIFIED RUN FAILURE")
            )
            results.append(row)
            save()
            if result.returncode and not diagnosed:
                raise RuntimeError(
                    f"Unclassified run failure: {name} capacity={capacity}"
                )
            return result.returncode

        for capacity in (16, 1):
            if run("baseline", source, capacity) != 0:
                raise RuntimeError(f"BASELINE FAILS at capacity {capacity}")
        if args.compile_failure_negative_control:
            run(
                "invalid-c-control",
                source + "\n#error EXPECTED_MUTATION_COMPILE_FAILURE\n",
                16,
            )
            raise RuntimeError("Compile failure negative control unexpectedly compiled")
        for name, pattern, replacement in MUTATIONS:
            changed = mutate(source, name, pattern, replacement)
            killed = [run(name, changed, capacity) != 0 for capacity in (16, 1)]
            if not any(killed):
                raise RuntimeError(f"SURVIVED: {name}")
            print(f"killed: {name}", flush=True)
    print("overwrite negative controls: all 10 killed", flush=True)


if __name__ == "__main__":
    main()
