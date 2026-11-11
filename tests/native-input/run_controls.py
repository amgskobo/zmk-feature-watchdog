"""Keep unchanged upstream mock UBSan failures separate from owner products."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

OWNER = Path(__file__).resolve().parents[2]
EXPECTED_CORE = "e5c9b6915b56801193e359dd9bad4a167ce0d1b8"
CASES = ("test", "watchdog", "studio", "studio_unsecured", "split_peripheral")


def main():
    sys.path.insert(0, str(OWNER / "tests"))
    from native_input import require_native_runtime_libraries

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.resolve(strict=True)
    output.mkdir()
    commands = []

    def run(argv, cwd, log, timeout=180):
        start = datetime.datetime.now(datetime.timezone.utc).isoformat()
        clock = time.monotonic_ns()
        result = subprocess.run(
            list(map(str, argv)),
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        log.write_text(result.stdout)
        commands.append(
            {
                "argv": list(map(str, argv)),
                "cwd": str(cwd),
                "exit": result.returncode,
                "started_utc": start,
                "elapsed_ns": time.monotonic_ns() - clock,
                "raw_output": str(log.relative_to(output)),
            }
        )
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        return result

    zmk = run(
        ["west", "list", "zmk", "-f", "{abspath}"],
        args.workspace,
        output / "selected-core.log",
    )
    assert zmk.returncode == 0, zmk.stdout
    core = Path(zmk.stdout.strip())
    head = run(
        ["git", "--no-optional-locks", "-C", core, "rev-parse", "HEAD"],
        args.workspace,
        output / "core-head.log",
    )
    assert head.returncode == 0 and head.stdout.strip() == EXPECTED_CORE, head.stdout
    inputs = {}
    for relative in [
        "app/module/drivers/kscan/kscan_mock.c",
        "app/module/include/dt-bindings/zmk/kscan_mock.h",
    ]:
        original = run(
            [
                "git",
                "--no-optional-locks",
                "-C",
                core,
                "show",
                EXPECTED_CORE + ":" + relative,
            ],
            args.workspace,
            output / (Path(relative).name + ".original.log"),
        )
        assert original.returncode == 0, original.stdout
        data = (core / relative).read_bytes()
        assert original.stdout.encode() == data, (
            "core control source differs: " + relative
        )
        inputs[relative] = hashlib.sha256(data).hexdigest()
    qualified = []
    for case in CASES:
        case_output = output / case
        case_output.mkdir()
        build = case_output / "build"
        command = [
            "west",
            "build",
            "-s",
            core / "app",
            "-b",
            "native_sim//zmk_test_mock",
            "-d",
            build,
            "-p",
            "-o=-j1",
            "--",
            "-DCONFIG_ASSERT=y",
            "-DZMK_CONFIG=" + str(OWNER / "tests" / case),
            "-DZMK_EXTRA_MODULES=" + str(OWNER),
            "-DEXTRA_DTC_OVERLAY_FILE="
            + str(OWNER / "tests/native-input/original-core.overlay"),
            "-DEXTRA_CONF_FILE=" + str(OWNER / "tests/native-input/original-core.conf"),
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DEXTRA_CFLAGS=-fsanitize=address,undefined;-fno-sanitize-recover=all",
            "-DEXTRA_LDFLAGS=-fsanitize=address,undefined;-fno-sanitize-recover=all",
        ]
        built = run(command, args.workspace, case_output / "build.log", timeout=300)
        assert built.returncode == 0, f"{case}: control must compile; see {case_output}"
        config = (build / "zephyr/.config").read_text().splitlines()
        assert "CONFIG_ZMK_KSCAN_MOCK_DRIVER=y" in config
        assert "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y" not in config
        assert "CONFIG_ASAN=y" in config and "CONFIG_ASAN_RECOVER=y" not in config
        assert "CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y" in config
        for line in (
            (OWNER / "tests" / case / "native_sim.conf").read_text().splitlines()
        ):
            if (
                line.startswith("CONFIG_ZMK_")
                and line.endswith("=y")
                and not line.startswith("CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=")
            ):
                assert line in config, (
                    f"{case}: original requested feature missing: {line}"
                )
        commands_db = json.loads((build / "compile_commands.json").read_text())
        mock = [
            entry for entry in commands_db if entry["file"].endswith("/kscan_mock.c")
        ]
        assert len(mock) == 1
        assert not any(
            "/tests/native-input/kscan.c" in entry["file"] for entry in commands_db
        )
        flags = mock[0].get("arguments") or shlex.split(mock[0]["command"])
        assert "-fsanitize=address,undefined" in flags
        assert "-fno-sanitize-recover=all" in flags
        assert "-fsanitize-recover=all" not in flags
        nsi = (build / "zephyr/NSI/nsi_config").read_text()
        link = next(
            (
                line
                for line in nsi.splitlines()
                if line.startswith("NSI_LINK_OPTIONS:=")
            ),
            "",
        )
        assert (
            "-fsanitize=address,undefined" in link
            and "-fno-sanitize-recover=all" in link
        )
        binary = build / "zephyr/zmk.exe"
        dynamic = run(
            ["readelf", "--dynamic", binary],
            args.workspace,
            case_output / "runtime-link.log",
        )
        assert dynamic.returncode == 0
        require_native_runtime_libraries(dynamic.stdout)
        symbols = run(
            ["nm", "--defined-only", binary],
            args.workspace,
            case_output / "symbols.log",
        )
        assert symbols.returncode == 0
        for declaration in json.loads(
            (OWNER / "tests/required_hooks.json").read_text()
        ):
            if declaration["hook"].startswith("watchdog_native_input_"):
                continue  # This named control intentionally uses the upstream backend.
            selected = (
                case in declaration["fixtures"]
                if "fixtures" in declaration
                else all(key + "=y" in config for key in declaration["configs"])
            )
            if selected:
                assert all(key + "=y" in config for key in declaration["configs"])
                assert not any(
                    key + "=y" in config for key in declaration.get("not_configs", [])
                )
                assert "owner_checked_" + declaration["hook"] in symbols.stdout
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for trial in range(1, 4):
            initial = case_output / str(trial)
            initial.mkdir()  # Same ELF, independent empty initial-flash state.
            result = run([binary], initial, initial / "raw.log", timeout=30)
            assert result.returncode != 0, (
                f"{case}: original core unexpectedly exited zero"
            )
            assert re.search(
                r"kscan_mock\.c:103(?::\d+)?: runtime error: left shift of 1 by 31 "
                r"places cannot be represented in type 'int'",
                result.stdout,
            ), f"{case}: not the named core shift rejection; see {initial}"
            qualified.append(
                {
                    "case": case,
                    "trial": trial,
                    "exit": result.returncode,
                    "binary_sha256": digest,
                    "classification": "EXPECTED original-core signed-shift rejection",
                }
            )
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
    for relative, digest in inputs.items():
        assert hashlib.sha256((core / relative).read_bytes()).hexdigest() == digest
    (output / "result.json").write_text(
        json.dumps(
            {
                "status": "PASS expected rejections only; not product PASS",
                "core": EXPECTED_CORE,
                "unchanged_core_inputs": inputs,
                "qualified_rejections": qualified,
                "source_rewriting": False,
                "sanitizer_suppression": False,
            },
            indent=2,
        )
        + "\n"
    )
    print("EXPECTED_CORE_MOCK_REJECTIONS fixtures=5 repeats=3 qualified=15", flush=True)


if __name__ == "__main__":
    main()
