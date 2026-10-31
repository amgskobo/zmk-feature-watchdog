# SPDX-License-Identifier: MIT
"""Actual zmk-watchdog-fork timeout header: numeric/ABI/coverage and identified fault checks."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


OWNER = Path(__file__).resolve().parents[1]
HEADER = OWNER / "src/watchdog_timeout.h"
UNIT = OWNER / "tests/timeout/test_timeout.c"


class TimeoutWidth(unittest.TestCase):
    def test_numeric_variants_and_measured_header_coverage(self):
        mode = os.environ.get("ZMK_TEST_MODE", "quick")
        self.assertIn(mode, ("quick", "full"))
        variants = [
            (
                "sanitized",
                [
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                ],
            )
        ]
        if mode == "full":
            variants += [
                ("optimized", ["-O2"]),
                ("coverage", ["-O0", "--coverage"]),
                ("32bit", ["-O2", "-m32"]),
            ]
        with tempfile.TemporaryDirectory(
            prefix="zmk-watchdog-fork-timeout-numeric-"
        ) as folder:
            root = Path(folder)
            for ticks in ("64", "32"):
                abi_flags = ["-DTIMER_TIMEOUT_32=1"] if ticks == "32" else []
                for variant, flags in variants:
                    with self.subTest(ticks=ticks, variant=variant):
                        work = root / (ticks + variant)
                        work.mkdir()
                        binary = work / "numeric"
                        subprocess.run(
                            [
                                "cc",
                                "-std=c11",
                                "-Wall",
                                "-Wextra",
                                "-Werror",
                                *flags,
                                *abi_flags,
                                "-I",
                                str(OWNER / "src"),
                                "-I",
                                str(OWNER / "tests/runtime"),
                                str(UNIT),
                                "-o",
                                str(binary),
                            ],
                            check=True,
                            cwd=work,
                            capture_output=True,
                            text=True,
                        )
                        result = subprocess.run(
                            [str(binary)], check=True, capture_output=True, text=True
                        )
                        print(
                            f"zmk-watchdog-fork timeout ticks{ticks} {variant}: {result.stdout.strip()}"
                        )
                        if variant == "coverage":
                            gcno = next(work.glob("*.gcno"))
                            report = subprocess.check_output(
                                ["gcov", "-b", "-c", "-o", str(gcno), str(UNIT)],
                                cwd=work,
                                text=True,
                            )
                            block = re.search(
                                "File '"
                                + re.escape(str(HEADER))
                                + r"'\n(.*?)(?=\nFile |\Z)",
                                report,
                                re.S,
                            )
                            self.assertIsNotNone(block, report)
                            for label in ("Lines executed", "Taken at least once"):
                                measured = re.search(
                                    label + r":100\.00% of ([0-9]+)", block[1]
                                )
                                self.assertIsNotNone(measured, report)
                                self.assertGreater(int(measured[1]), 0, report)
                            print(
                                f"zmk-watchdog-fork timeout ticks{ticks} measured coverage: {block[0].strip()}"
                            )

    def test_width_and_identified_faults(self):
        original = HEADER.read_text()
        controls = {
            "clock-wrap": (
                "return (uint32_t)(now - started) < delay;",
                "return now >= started && (now - started) < delay;",
            ),
            "clock-boundary": (
                "return (uint32_t)(now - started) < delay;",
                "return (uint32_t)(now - started) <= delay;",
            ),
            "floor-instead-of-ceil": (
                "return ((uint32_t)ms * hz + 999U) / 1000U;",
                "return ((uint32_t)ms * hz) / 1000U;",
            ),
            "unchecked-product": ("if (ms <= (UINT32_MAX - 999U) / hz)", "if (1)"),
            "lost-fraction": ("(remainder * (hz % 1000U) + 999U) / 1000U", "0U"),
            "lost-seconds": (
                "return (k_ticks_t)seconds * hz + fraction;",
                "return fraction;",
            ),
            "wide-division": (
                "if (ms <= (UINT32_MAX - 999U) / hz) {\n"
                "        return ((uint32_t)ms * hz + 999U) / 1000U;\n    }",
                "return (k_ticks_t)(((uint64_t)ms * hz + 999U) / 1000U);",
            ),
        }
        with tempfile.TemporaryDirectory(
            prefix="zmk-watchdog-fork-timeout-width-"
        ) as folder:
            root = Path(folder)
            for ticks in ("64", "32"):
                abi_flags = ["-DTIMER_TIMEOUT_32=1"] if ticks == "32" else []
                for name in ("original", *controls):
                    with self.subTest(ticks=ticks, control=name):
                        text = original
                        if name != "original":
                            before, after = controls[name]
                            self.assertEqual(text.count(before), 1, before)
                            text = text.replace(before, after)
                        include = root / (name + ticks)
                        include.mkdir()
                        (include / HEADER.name).write_text(text)
                        obj, binary = include / "probe.o", include / "numeric"
                        common = [
                            "cc",
                            "-m32",
                            "-O2",
                            "-std=c11",
                            *abi_flags,
                            "-I",
                            str(include),
                            "-I",
                            str(OWNER / "tests/runtime"),
                        ]
                        subprocess.run(
                            [
                                *common,
                                "-DTIMER_TIMEOUT_WIDTH_PROBE=1",
                                "-c",
                                str(UNIT),
                                "-o",
                                str(obj),
                            ],
                            check=True,
                            capture_output=True,
                            text=True,
                        )
                        symbols = subprocess.check_output(
                            ["nm", "--undefined-only", str(obj)], text=True
                        )
                        found = re.findall(
                            r"__(?:u?divdi3|u?moddi3|u?divmoddi4|muldi3)", symbols
                        )
                        if name == "wide-division":
                            self.assertTrue(found, symbols)
                            print(
                                f"ticks{ticks} compiled wide control rejected: {found}"
                            )
                        elif name == "original":
                            self.assertEqual(found, [], symbols)
                        subprocess.run(
                            [*common, str(UNIT), "-o", str(binary)],
                            check=True,
                            capture_output=True,
                            text=True,
                        )
                        for repeat in range(3):
                            result = subprocess.run(
                                [str(binary)], capture_output=True, text=True
                            )
                            if name in ("original", "wide-division"):
                                self.assertEqual(result.returncode, 0, result.stderr)
                            else:
                                self.assertEqual(result.returncode, -6, result.stderr)
                                self.assertRegex(
                                    result.stderr,
                                    r"[^\n]+:[0-9]+: [^\n]+: Assertion .+ failed\.",
                                )
                                print(
                                    f"ticks{ticks} {name} repeat{repeat + 1}: {result.stderr.strip()}"
                                )


if __name__ == "__main__":
    unittest.main()
