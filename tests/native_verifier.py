"""Keep raw native runs, require real exit status and each registered test result."""

from pathlib import Path
import datetime
import hashlib
import json
import re
import subprocess
import time

from native_input import (
    load_input_oracles,
    require_native_runtime_libraries,
    verify_input_build,
    verify_input_trace,
)


def enabled_config(path):
    return set(re.findall(r"^(CONFIG_\w+)=y$", path.read_text(), re.M))


def required_hooks(binary, fixture, tests):
    declarations = json.loads((tests / "required_hooks.json").read_text())
    built = enabled_config(binary.parent / ".config")
    requested = enabled_config(fixture / "native_sim.conf")
    selected = []
    for declaration in declarations:
        configs = declaration["configs"]
        hook = declaration["hook"]
        if not configs or not re.fullmatch(r"\w+", hook):
            raise AssertionError("Invalid checked-init declaration")
        fixtures = declaration.get("fixtures")
        if fixtures is not None:
            if not fixtures:
                raise AssertionError("Invalid empty checked-init fixture selection")
            if fixture.relative_to(tests).as_posix() not in fixtures:
                continue
        elif not all(config in built | requested for config in configs):
            continue
        excluded = declaration.get("not_configs", [])
        if not all(config in built for config in configs) or any(
            config in built for config in excluded
        ):
            raise AssertionError(
                f"{fixture.name}: required configuration {configs}; excluded {excluded}"
            )
        selected.append(hook)
    return selected


def verify_native(output: Path, tests: Path):
    binaries = sorted(output.rglob("zmk.exe"))
    if not binaries:
        raise AssertionError(f"No native binary below {output}")
    expected = {
        path.parent.relative_to(tests).as_posix()
        for path in tests.rglob("native_sim.keymap")
    }

    def case_name(binary):
        parts = binary.parent.parent.relative_to(output).parts
        if parts and parts[0] == "tests":
            parts = parts[1:]
        return Path(*parts).as_posix()

    actual = {case_name(binary) for binary in binaries}
    if not expected:
        raise AssertionError(f"No native fixture inputs below {tests}")
    if expected - actual:
        raise AssertionError(f"missing native fixtures: {sorted(expected - actual)}")
    if actual - expected:
        raise AssertionError(f"unknown native fixtures: {sorted(actual - expected)}")
    input_oracles = None
    if any(
        "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE"
        in enabled_config(path / "native_sim.conf")
        for path in (tests / case for case in expected)
    ):
        input_oracles = load_input_oracles(tests, expected)
    identity_nonces = set()
    for binary in binaries:
        symbols = subprocess.check_output(
            ["nm", "--defined-only", str(binary)], text=True
        )
        hooks = re.findall(r"\bowner_checked_(\w+)$", symbols, re.M)
        case = case_name(binary)
        fixture = tests / case
        if not (fixture / "keycode_events.snapshot").is_file():
            raise AssertionError(f"Unknown native fixture: {case}")
        missing = set(required_hooks(binary, fixture, tests)) - set(hooks)
        if missing:
            raise AssertionError(f"{case}: missing checked init {sorted(missing)}")
        if input_oracles is not None:
            verify_input_build(binary, input_oracles[case])
        destination = binary.parent.parent / "verified-runs"
        destination.mkdir(exist_ok=False)
        if input_oracles is not None:
            dynamic = subprocess.run(
                ["readelf", "--dynamic", str(binary)],
                capture_output=True,
                text=True,
                check=True,
            )
            (destination / "runtime-link.log").write_text(
                dynamic.stdout + dynamic.stderr
            )
            require_native_runtime_libraries(dynamic.stdout)
        binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
        if "CONFIG_ZMK_WATCHDOG_DIAGNOSTICS" in enabled_config(
            binary.parent / ".config"
        ):
            identities = list(
                binary.parent.parent.rglob("watchdog-build-identity.json")
            )
            if len(identities) != 1:
                raise AssertionError(f"{case}: missing/duplicate diagnostic identity")
            identity = json.loads(identities[0].read_text())
            nonce = identity["configuration_nonce"]
            if not re.fullmatch(r"[0-9a-f]{32}", nonce) or nonce in identity_nonces:
                raise AssertionError(f"{case}: invalid/reused configuration nonce")
            identity_nonces.add(nonce)
            if input_oracles is not None:
                for name in ["tests/native-input/kscan.c", "tests/native-input/word.h"]:
                    actual_digest = hashlib.sha256(
                        (tests.parent / name).read_bytes()
                    ).hexdigest()
                    if identity["inputs"].get(name) != actual_digest:
                        raise AssertionError(
                            f"{case}: input fixture missing from build identity: {name}"
                        )
            (destination / "watchdog-build-manifest.json").write_text(
                json.dumps({**identity, "elf_sha256": binary_hash}, indent=2) + "\n"
            )
        (destination / "inputs.json").write_text(
            json.dumps(
                {
                    "binary_sha256": binary_hash,
                    "checked_init_hooks": hooks,
                    "fixtures": {
                        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in (
                            fixture / "native_sim.keymap",
                            fixture / "native_sim.conf",
                            fixture / "events.patterns",
                            fixture / "keycode_events.snapshot",
                            tests / "required_hooks.json",
                        )
                    },
                },
                indent=2,
            )
            + "\n"
        )
        for trial in range(1, 4):
            run = destination / str(trial)
            run.mkdir()  # independent empty initial-flash directory, same binary/input
            started = datetime.datetime.now(datetime.timezone.utc).isoformat()
            clock = time.monotonic_ns()
            result = subprocess.run(
                [str(binary)],
                cwd=run,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            raw = result.stdout
            (run / "raw.log").write_text(raw)
            (run / "command.json").write_text(
                json.dumps(
                    {
                        "argv": [str(binary)],
                        "cwd": str(run),
                        "exit": result.returncode,
                        "start_utc": started,
                        "end_utc": datetime.datetime.now(
                            datetime.timezone.utc
                        ).isoformat(),
                        "elapsed_ns": time.monotonic_ns() - clock,
                    },
                    indent=2,
                )
                + "\n"
            )
            if result.returncode:
                raise AssertionError(
                    f"{case}: native exit {result.returncode}; see {run}"
                )
            # Recoverable faults may preserve exit 0, checked initialization and
            # the expected snapshot. Match actual diagnostic formats rather than
            # ordinary sanitizer option names or application status markers.
            if re.search(
                r"^(?:(?:==\d+==)?(?:ERROR|WARNING|SUMMARY): "
                r"(?:Address|Leak|Memory|Thread|UndefinedBehavior)Sanitizer:"
                r"|(?:Address|UndefinedBehavior)Sanitizer:DEADLYSIGNAL"
                r"|[^\n]+:\d+(?::\d+)?: runtime error:)",
                raw,
                re.M,
            ):
                raise AssertionError(f"{case}: sanitizer diagnostic; see {run}")
            if re.search(r"\bFAIL:", raw):
                raise AssertionError(f"{case}: failed owner assertion; see {run}")
            input_observation = None
            if input_oracles is not None:
                input_observation = verify_input_trace(raw, input_oracles[case])
                (run / "input-boundary.json").write_text(
                    json.dumps(input_observation, indent=2) + "\n"
                )
            for hook in hooks:
                # This newly-added asynchronous probe cannot execute after the
                # three original APP99 tests permanently wedge sysworkq and exit.
                # The case-specific terminal/zero-input proof above is mandatory;
                # every original or unknown checked hook remains checked below.
                if (
                    hook == "watchdog_native_input_complete"
                    and input_observation is not None
                    and input_observation["input_status"] == "NOT APPLICABLE"
                ):
                    continue
                values = re.findall(
                    r"OWNER_TEST_RESULT " + re.escape(hook) + r"=(-?\d+)", raw
                )
                if values != ["0"]:
                    raise AssertionError(
                        f"{case}: {hook} completion {values}; see {run}"
                    )
            if re.search(
                r"(?:ASSERTION FAIL|ZEPHYR FATAL ERROR|PROJECT EXECUTION FAILED)", raw
            ):
                raise AssertionError(f"{case}: fatal assertion; see {run}")
            stripped = re.sub(r".*> ", "", raw)
            filtered = subprocess.run(
                ["sed", "-n", "-f", str(fixture / "events.patterns")],
                input=stripped,
                text=True,
                capture_output=True,
                check=True,
            )
            actual = run / "events.log"
            actual.write_text(filtered.stdout)
            diff = subprocess.run(
                ["diff", "-auZ", str(fixture / "keycode_events.snapshot"), str(actual)],
                text=True,
                capture_output=True,
            )
            (run / "snapshot.diff").write_text(diff.stdout + diff.stderr)
            if diff.returncode:
                raise AssertionError(f"{case}: unchanged snapshot mismatch; see {run}")
        if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
            raise AssertionError(f"{case}: binary changed between repeats")
        print(
            f"Verified {case}: identical binary, raw ordered output, registered tests={len(hooks)}, repeats=3",
            flush=True,
        )


if __name__ == "__main__":
    import argparse
    import hashlib
    import json
    import shutil

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retained-build", required=True, type=Path)
    parser.add_argument("--new-output", required=True, type=Path)
    args = parser.parse_args()
    # Preserve old failures; reuse bytes only in a fresh independently logged copy.
    shutil.copytree(
        args.retained_build,
        args.new_output,
        ignore=shutil.ignore_patterns("verified-runs"),
    )
    hashes = {}
    for binary in args.new_output.rglob("zmk.exe"):
        binary.chmod(0o755)
        hashes[str(binary.relative_to(args.new_output))] = hashlib.sha256(
            binary.read_bytes()
        ).hexdigest()
    (args.new_output / "retained-binary-hashes.json").write_text(
        json.dumps(hashes, indent=2)
    )
    verify_native(args.new_output.resolve(), Path(__file__).resolve().parent)
