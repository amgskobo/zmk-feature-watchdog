"""Use real synthetic ELF programs to check native-result admission."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from native_verifier import verify_native


class NativeVerifierContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tests = self.root / "fixtures"
        fixture = self.tests / "case"
        fixture.mkdir(parents=True)
        (fixture / "native_sim.keymap").write_text(
            "/* Synthetic native fixture marker. */\n"
        )
        (fixture / "native_sim.conf").write_text("CONFIG_OWNER_TEST=y\n")
        (fixture / "events.patterns").write_text("/^EVENT /p\n")
        (fixture / "keycode_events.snapshot").write_text("EVENT 1\n")
        (self.tests / "required_hooks.json").write_text(
            json.dumps([{"configs": ["CONFIG_OWNER_TEST"], "hook": "control"}])
        )
        self.output = self.root / "build"
        binary_dir = self.output / "case" / "zephyr"
        binary_dir.mkdir(parents=True)
        (binary_dir / ".config").write_text("CONFIG_OWNER_TEST=y\n")
        self.binary = binary_dir / "zmk.exe"

    def compile(
        self,
        *,
        hook=True,
        reported=0,
        process_exit=0,
        fatal=False,
        failed_log=False,
        sanitizer=None,
        extra_output=None,
    ):
        source = self.root / "program.c"
        body = "#include <stdio.h>\n"
        if sanitizer == "address":
            body += "static volatile unsigned char guarded[1];\n"
        if hook:
            body += (
                "int owner_checked_control(void) { "
                f'puts("OWNER_TEST_RESULT control={reported}"); return 0; }}\n'
            )
        body += "int main(void) { "
        if hook:
            body += "owner_checked_control(); "
        if fatal:
            body += 'puts("ASSERTION FAIL"); '
        if failed_log:
            body += 'puts("<err> owner: FAIL: controlled assertion"); '
        if extra_output is not None:
            body += "puts(" + json.dumps(extra_output) + "); "
        if sanitizer == "undefined":
            body += "volatile int shift = 31; volatile int value = 1; "
            body += "value = value << shift; (void)value; "
        elif sanitizer == "address":
            body += (
                "volatile unsigned index = 1; guarded[index] = 7; (void)guarded[0]; "
            )
        body += f'puts("EVENT 1"); return {process_exit}; }}\n'
        source.write_text(body)
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                *(
                    [
                        "-fsanitize=" + sanitizer,
                        "-fsanitize-recover=" + sanitizer,
                        "-fno-pie",
                        "-no-pie",
                    ]
                    if sanitizer
                    else []
                ),
                str(source),
                "-o",
                str(self.binary),
            ],
            check=True,
        )

    def test_positive_repeats_preserve_raw_output(self):
        self.compile()
        verify_native(self.output, self.tests)
        runs = self.binary.parent.parent / "verified-runs"
        self.assertEqual(
            sorted(path.name for path in runs.iterdir() if path.is_dir()),
            ["1", "2", "3"],
        )
        for trial in range(1, 4):
            self.assertEqual(
                (runs / str(trial) / "events.log").read_text(), "EVENT 1\n"
            )
            self.assertIn(
                "OWNER_TEST_RESULT control=0",
                (runs / str(trial) / "raw.log").read_text(),
            )

    def test_missing_checked_init_is_not_snapshot_success(self):
        self.compile(hook=False)
        with self.assertRaisesRegex(AssertionError, "missing checked init"):
            verify_native(self.output, self.tests)

    def test_failed_init_is_not_snapshot_success(self):
        self.compile(reported=-22)
        with self.assertRaisesRegex(AssertionError, "completion"):
            verify_native(self.output, self.tests)

    def test_real_process_exit_is_required(self):
        self.compile(process_exit=7)
        with self.assertRaisesRegex(AssertionError, "native exit 7"):
            verify_native(self.output, self.tests)

    def test_requested_test_cannot_be_disabled_in_build(self):
        self.compile()
        (self.binary.parent / ".config").write_text("# CONFIG_OWNER_TEST is not set\n")
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_logged_assertion_cannot_pass_with_zero_init_result(self):
        self.compile(failed_log=True)
        with self.assertRaisesRegex(AssertionError, "failed owner assertion"):
            verify_native(self.output, self.tests)

    def test_fixture_requires_its_test_even_when_both_configs_omit_it(self):
        self.compile()
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["case"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "hook": "control",
                    }
                ]
            )
        )
        (self.tests / "case/native_sim.conf").write_text("")
        (self.binary.parent / ".config").write_text("")
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_fixture_role_cannot_be_replaced_by_a_wrong_built_role(self):
        self.compile()
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["case"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "not_configs": ["CONFIG_ROLE_CENTRAL"],
                        "hook": "control",
                    }
                ]
            )
        )
        (self.binary.parent / ".config").write_text(
            "CONFIG_OWNER_TEST=y\nCONFIG_ROLE_CENTRAL=y\n"
        )
        with self.assertRaisesRegex(AssertionError, "required configuration"):
            verify_native(self.output, self.tests)

    def test_unrelated_fixture_declaration_does_not_require_a_hook(self):
        self.compile(hook=False)
        (self.tests / "required_hooks.json").write_text(
            json.dumps(
                [
                    {
                        "fixtures": ["other"],
                        "configs": ["CONFIG_OWNER_TEST"],
                        "hook": "control",
                    }
                ]
            )
        )
        verify_native(self.output, self.tests)

    def test_missing_native_fixture_cannot_be_a_partial_success(self):
        self.compile()
        other = self.tests / "missing-case"
        other.mkdir()
        (other / "native_sim.keymap").write_text("/* A required second scenario. */\n")
        with self.assertRaisesRegex(AssertionError, "missing native fixtures"):
            verify_native(self.output, self.tests)

    def test_fatal_with_zero_exit_is_rejected(self):
        self.compile(fatal=True)
        with self.assertRaisesRegex(AssertionError, "fatal assertion"):
            verify_native(self.output, self.tests)

    def test_recovering_ubsan_with_zero_exit_is_rejected(self):
        self.compile(sanitizer="undefined")
        self.check_recovering_sanitizer("runtime error:")

    def test_recovering_asan_with_zero_exit_is_rejected(self):
        self.compile(sanitizer="address")
        with patch.dict(os.environ, {"ASAN_OPTIONS": "halt_on_error=0:detect_leaks=0"}):
            self.check_recovering_sanitizer("ERROR: AddressSanitizer:")

    def test_sanitizer_names_and_normal_markers_are_not_diagnostics(self):
        self.compile(
            extra_output="CONFIG_ASAN=y\nCONFIG_UBSAN=y\nAddressSanitizer enabled\n"
            "OWNER_TEST_RESULT UndefinedBehaviorSanitizer=0\n"
            "runtime error: none (synthetic application marker)"
        )
        verify_native(self.output, self.tests)

    def check_recovering_sanitizer(self, diagnostic):
        # Establish the runtime fault and zero exit independently of admission;
        # a compile/setup failure cannot qualify as a rejected control.
        result = subprocess.run(
            [str(self.binary)],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        (self.root / "sanitizer-control.log").write_text(result.stdout)
        self.assertEqual(result.returncode, 0)
        self.assertIn(diagnostic, result.stdout)
        with self.assertRaisesRegex(AssertionError, "sanitizer diagnostic"):
            verify_native(self.output, self.tests)
        run = self.binary.parent.parent / "verified-runs/1"
        self.assertEqual(json.loads((run / "command.json").read_text())["exit"], 0)
        self.assertIn(diagnostic, (run / "raw.log").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
