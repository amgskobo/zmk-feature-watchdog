"""Optional real-ELF CLI acceptance; no device or transport acceptance."""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/decode_trace.py"


class RealElfRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        elf = os.environ.get("WATCHDOG_DECODER_ELF")
        manifest = os.environ.get("WATCHDOG_DECODER_MANIFEST")
        if not elf and not manifest:
            raise unittest.SkipTest("select a real Watchdog ELF and its manifest")
        if not elf or not manifest:
            raise ValueError("both WATCHDOG_DECODER_ELF and MANIFEST are required")
        cls.elf = Path(elf)
        cls.manifest = Path(manifest)
        cls.identity = json.loads(cls.manifest.read_text())
        if (
            cls.identity["elf_sha256"]
            != hashlib.sha256(cls.elf.read_bytes()).hexdigest()
        ):
            raise ValueError("selected ELF does not match its manifest")
        symbols = subprocess.check_output(
            ["arm-zephyr-eabi-nm", "-n", str(cls.elf)], text=True
        )
        cls.pc = next(
            int(line.split()[0], 16)
            for line in symbols.splitlines()
            if line.endswith(" zmk_watchdog_diagnostics_build_tag")
        )
        cls.output = None
        if directory := os.environ.get("WATCHDOG_DECODER_CLI_OUTPUT"):
            cls.output = Path(directory)
            cls.output.mkdir(parents=True)

    def incident(self, incident_id=1):
        diagnostics = bytearray(80)
        diagnostics[0] = 1
        struct.pack_into("<I", diagnostics, 4, int(self.identity["build_tag"], 16))
        return {
            "id": incident_id,
            "diagnostics": list(diagnostics),
            "fatal": {"pc": self.pc, "lr": self.pc + 3, "context": [0] * 24},
        }

    def run_report(self, label, report, manifest=None, exit_code=0):
        with tempfile.TemporaryDirectory() as temporary:
            directory = self.output or Path(temporary)
            prefix = self._testMethodName + "-" + label
            payload = directory / (prefix + ".json")
            payload.write_text(json.dumps(report))
            selected = self.manifest
            if manifest is not None:
                selected = directory / (prefix + "-manifest.json")
                selected.write_text(json.dumps(manifest))
            command = [
                sys.executable,
                str(SCRIPT),
                str(payload),
                "--elf",
                str(self.elf),
                "--build-manifest",
                str(selected),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            if self.output:
                (directory / (prefix + ".stdout")).write_text(result.stdout)
                (directory / (prefix + ".stderr")).write_text(result.stderr)
                (directory / (prefix + "-command.json")).write_text(
                    json.dumps({"command": command, "exit": result.returncode})
                )
            self.assertEqual(result.returncode, exit_code, result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            return json.loads(result.stdout) if exit_code == 0 else result.stderr

    def test_malformed_fields_keep_independent_symbols_and_next_record(self):
        for label in ["fatal-context", "incident", "detail", "trace", "freeze"]:
            with self.subTest(case=label):
                raw = self.incident()
                expected_symbols = 2
                if label == "fatal-context":
                    raw["fatal"]["context"] = [1]
                elif label == "incident":
                    raw = None
                    expected_symbols = 0
                elif label == "detail":
                    raw["fatal"] = "bad"
                    expected_symbols = 0
                elif label == "trace":
                    del raw["fatal"]
                    capture = bytearray(192)
                    capture[:2] = bytes([1, 16])
                    struct.pack_into("<I", capture, 8, self.pc + 3)
                    raw.update(captureFlags=4, captureContext=list(capture))
                    raw["freeze"] = {"trace": [1]}
                    expected_symbols = 1
                else:
                    raw["freeze"] = "bad"
                report = {
                    "format": "zmk-watchdog-report-v1",
                    "incidents": [raw, self.incident(2)],
                }
                result = self.run_report(label, report)["incidents"]
                self.assertEqual(result[0]["raw_incident"], raw)
                self.assertTrue(result[0]["error"])
                self.assertEqual(len(result[0].get("symbols", [])), expected_symbols)
                self.assertEqual(result[1]["id"], 2)
                self.assertNotIn("error", result[1])
                self.assertEqual(len(result[1]["symbols"]), 2)
                self.assertIn(
                    "zmk_watchdog_diagnostics_build_tag",
                    "\n".join(result[1]["symbols"][0]["symbol"]),
                )
                if expected_symbols:
                    self.assertIn(
                        "zmk_watchdog_diagnostics_build_tag",
                        "\n".join(result[0]["symbols"][0]["symbol"]),
                    )

    def test_mismatched_build_refuses_every_symbol(self):
        raw = self.incident()
        raw["fatal"]["context"] = [1]
        raw["freeze"] = "bad"
        wrong = dict(self.identity)
        wrong["build_tag"] = f"{int(wrong['build_tag'], 16) ^ 0xFFFFFFFF:08x}"
        result = self.run_report(
            "wrong-tag",
            {"format": "zmk-watchdog-report-v1", "incidents": [raw]},
            manifest=wrong,
        )["incidents"][0]
        self.assertEqual(result["raw_incident"], raw)
        self.assertIn("build tag differs", result["error"])
        self.assertNotIn("symbols", result)

    def test_invalid_shapes_are_usage_errors(self):
        for label, report, manifest in [
            ("report", None, None),
            ("incidents", {"format": "zmk-watchdog-report-v1", "incidents": {}}, None),
            ("manifest", {"format": "zmk-watchdog-report-v1"}, []),
        ]:
            with self.subTest(case=label):
                error = self.run_report(label, report, manifest, exit_code=2)
                self.assertIn("error:", error)


if __name__ == "__main__":
    unittest.main()
