"""Boundary and rejection cases for the explicit owner input fixture."""

import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from native_input import (
    load_input_oracles,
    require_native_runtime_libraries,
    verify_input_build,
    verify_input_trace,
)

TESTS = Path(__file__).resolve().parent
CASES = {"test", "watchdog", "studio", "studio_unsecured", "split_peripheral"}
TRACE = (
    "WATCHDOG_INPUT_EVENT index=0 word=800a0000 row=0 column=0 pressed=1 delay_ms=10\n"
    "WATCHDOG_INPUT_EVENT index=1 word=000a0000 row=0 column=0 pressed=0 delay_ms=10\n"
    "WATCHDOG_INPUT_COMPLETE count=2\n"
    "OWNER_TEST_RESULT watchdog_native_input_complete=0\n"
)
HID = (
    "zmk: on_keymap_binding_pressed: position 0 keycode 0x70004\n"
    "zmk: on_keymap_binding_released: position 0 keycode 0x70004\n"
)
PRE_MAIN = {
    "watchdog": "PASS: watchdog_freeze_detect\nOWNER_TEST_RESULT watchdog_test_init=0\n",
    "studio": "PASS: watchdog_rpc_inject_freeze_recorded\nOWNER_TEST_RESULT watchdog_rpc_test_init=0\n",
    "studio_unsecured": "PASS: watchdog_rpc_inject_freeze_recorded\nOWNER_TEST_RESULT watchdog_rpc_test_init=0\n",
}


class NativeInputContract(unittest.TestCase):
    def test_unsigned_word_boundaries(self):
        output = Path(tempfile.mkdtemp(prefix="watchdog-input-word-"))
        print(f"Input word evidence: {output}", flush=True)
        command = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O1",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-pie",
            "-no-pie",
            str(TESTS / "native-input/word_test.c"),
            "-o",
            str(output / "words"),
        ]
        built = subprocess.run(command, capture_output=True, text=True)
        (output / "build.log").write_text(built.stdout + built.stderr)
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        for trial in range(1, 4):
            result = subprocess.run(
                [str(output / "words")], capture_output=True, text=True
            )
            (output / f"run-{trial}.log").write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout, "WATCHDOG_INPUT_WORD_PASS cases=7\n")
            self.assertEqual(result.stderr, "")

    def test_literal_order_and_role_oracles(self):
        oracles = load_input_oracles(TESTS, CASES)
        verify_input_trace(TRACE + HID, oracles["test"])
        verify_input_trace(TRACE, oracles["split_peripheral"])
        self.assertEqual(
            oracles["watchdog"]["configured_events"], oracles["test"]["events"]
        )
        self.assertEqual(oracles["studio"], oracles["studio_unsecured"])
        self.assertEqual(len(oracles["studio"]["configured_events"]), 4)
        for case, raw in PRE_MAIN.items():
            result = verify_input_trace(raw, oracles[case])
            self.assertEqual(result["input_status"], "NOT APPLICABLE")

    def test_wrong_missing_duplicate_and_reordered_callbacks_reject(self):
        oracle = load_input_oracles(TESTS, CASES)["test"]
        traces = [
            TRACE.replace("word=800a0000", "word=000a0000"),
            TRACE.replace("pressed=1", "pressed=0"),
            TRACE.replace("row=0", "row=1", 1),
            TRACE.replace("delay_ms=10", "delay_ms=9", 1),
            TRACE.splitlines(keepends=True)[0] + TRACE,
            "".join(TRACE.splitlines(keepends=True)[1:]),
            "".join(reversed(TRACE.splitlines(keepends=True)[:2]))
            + TRACE.splitlines(keepends=True)[2],
        ]
        for trace in traces:
            with self.subTest(trace=trace):
                with self.assertRaisesRegex(AssertionError, "ordered callback trace"):
                    verify_input_trace(trace + HID, oracle)

    def test_completion_and_actual_keymap_hid_cannot_be_omitted(self):
        oracle = load_input_oracles(TESTS, CASES)["test"]
        for trace in [
            TRACE.replace("WATCHDOG_INPUT_COMPLETE count=2\n", ""),
            TRACE.replace("count=2", "count=0"),
            TRACE + "WATCHDOG_INPUT_COMPLETE count=2\n",
        ]:
            with self.subTest(trace=trace):
                with self.assertRaisesRegex(AssertionError, "completion mismatch"):
                    verify_input_trace(trace + HID, oracle)
        with self.assertRaisesRegex(AssertionError, "completion precedes callbacks"):
            verify_input_trace(
                "WATCHDOG_INPUT_COMPLETE count=2\n"
                + TRACE.split("WATCHDOG_INPUT_COMPLETE")[0]
                + HID,
                oracle,
            )
        for hid in [
            "",
            HID.splitlines(keepends=True)[0],
            HID.replace("0x70004", "0x70005"),
        ]:
            with self.subTest(hid=hid):
                with self.assertRaisesRegex(AssertionError, "keymap/HID trace"):
                    verify_input_trace(TRACE + hid, oracle)
        with self.assertRaisesRegex(AssertionError, "keymap/HID trace"):
            verify_input_trace(
                TRACE + HID, load_input_oracles(TESTS, CASES)["split_peripheral"]
            )
        with self.assertRaisesRegex(AssertionError, "completion result"):
            verify_input_trace(
                TRACE.replace(
                    "OWNER_TEST_RESULT watchdog_native_input_complete=0\n", ""
                )
                + HID,
                oracle,
            )
        oracles = load_input_oracles(TESTS, CASES)
        for case, raw in PRE_MAIN.items():
            for wrong in [
                raw.replace("PASS:", "MISSING:"),
                raw.replace("=0", "=-1"),
                raw + raw.splitlines(keepends=True)[-1],
            ]:
                with self.subTest(case=case, wrong=wrong):
                    with self.assertRaisesRegex(AssertionError, "terminal evidence"):
                        verify_input_trace(wrong, oracles[case])
            for extra in [
                TRACE,
                HID,
                "Welcome to ZMK!\n",
                "WATCHDOG_INPUT_EVENT partial\n",
            ]:
                with self.subTest(case=case, extra=extra):
                    with self.assertRaisesRegex(
                        AssertionError, "pre-main outcome contains"
                    ):
                        verify_input_trace(extra + raw, oracles[case])
            with self.assertRaisesRegex(AssertionError, "terminal ordering"):
                verify_input_trace(
                    raw + "unclassified terminal output\n", oracles[case]
                )

    def test_oracle_selection_must_cover_exact_five_fixtures(self):
        with self.assertRaisesRegex(AssertionError, "fixture selection mismatch"):
            load_input_oracles(TESTS, CASES - {"studio_unsecured"})
        oracle = copy.deepcopy(load_input_oracles(TESTS, CASES)["test"])
        oracle["events"][0][4] = 0
        with self.assertRaisesRegex(AssertionError, "ordered callback trace"):
            verify_input_trace(TRACE + HID, oracle)
        data = load_input_oracles(TESTS, CASES)
        changed = copy.deepcopy(data)
        changed["test"]["boundary"] = "pre_main_freeze"
        with patch.object(Path, "read_text", return_value=json.dumps(changed)):
            with self.assertRaisesRegex(AssertionError, "case boundary mismatch"):
                load_input_oracles(TESTS, CASES)
        changed = copy.deepcopy(data)
        changed["studio"]["terminal_hook"] = "watchdog_native_input_complete"
        with patch.object(Path, "read_text", return_value=json.dumps(changed)):
            with self.assertRaisesRegex(
                AssertionError, "terminal declaration mismatch"
            ):
                load_input_oracles(TESTS, CASES)

    def test_strict_build_profile_rejects_missing_ubsan_and_later_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            zephyr = build / "zephyr"
            zephyr.mkdir()
            (zephyr / ".config").write_text(
                "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y\n"
                "CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y\nCONFIG_ASAN=y\n"
            )
            (zephyr / "NSI").mkdir()
            (zephyr / "NSI/nsi_config").write_text(
                "NSI_LINK_OPTIONS:=-fsanitize=address,undefined -fno-sanitize-recover=all\n"
            )
            (zephyr / "zephyr.dts").write_text(
                'compatible = "amgskobo,watchdog-test-kscan";\n'
            )
            source = "/synthetic-owner/tests/native-input/kscan.c"
            for flags, accepted in [
                (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"], True),
                (["-fsanitize=address,undefined"], False),
                (["-fsanitize=address", "-fno-sanitize-recover=all"], False),
                (["-fsanitize=address,undefined", "-fno-sanitize=all"], False),
                (
                    ["-fsanitize=address,undefined", "-fsanitize-recover=undefined"],
                    False,
                ),
                (
                    [
                        "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all",
                        "-fsanitize-recover=all",
                    ],
                    False,
                ),
            ]:
                with self.subTest(flags=flags):
                    (build / "compile_commands.json").write_text(
                        json.dumps(
                            [
                                {
                                    "file": source,
                                    "arguments": ["cc", *flags, "-c", source],
                                }
                            ]
                        )
                    )
                    if accepted:
                        verify_input_build(zephyr / "zmk.exe")
                    else:
                        with self.assertRaisesRegex(
                            AssertionError, "strict compiler profile"
                        ):
                            verify_input_build(zephyr / "zmk.exe")
            (zephyr / "NSI/nsi_config").write_text(
                "NSI_LINK_OPTIONS:=-fsanitize=address\n"
            )
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "file": source,
                            "arguments": [
                                "cc",
                                "-fsanitize=address,undefined",
                                "-fno-sanitize-recover=all",
                                "-c",
                                source,
                            ],
                        }
                    ]
                )
            )
            with self.assertRaisesRegex(AssertionError, "final-runner link profile"):
                verify_input_build(zephyr / "zmk.exe")
            require_native_runtime_libraries(
                "Shared library: [libasan.so.8]\nShared library: [libubsan.so.1]\n"
            )
            with self.assertRaisesRegex(AssertionError, "runtime dependency missing"):
                require_native_runtime_libraries("Shared library: [libasan.so.8]\n")
