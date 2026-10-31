"""A damaged optional capture must not discard independent fatal evidence."""

from contextlib import redirect_stdout
import hashlib
import importlib.util
from io import StringIO
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[2] / "tools/decode_trace.py"
SPEC = importlib.util.spec_from_file_location("capture_recovery_decoder", SCRIPT)
decoder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(decoder)


class CaptureRecoveryTests(unittest.TestCase):
    def incident(self, capture, flags=4, incident_id=1):
        diagnostics = bytearray(80)
        diagnostics[0] = 1
        struct.pack_into("<I", diagnostics, 4, 0x1234ABCD)
        return {
            "id": incident_id,
            "type": 1,
            "captureFlags": flags,
            "captureContext": capture,
            "diagnostics": list(diagnostics),
            "fatal": {
                "reason": 3,
                "pc": 0x1000,
                "lr": 0x2001,
                "threadName": "synthetic",
                "context": list(struct.pack("<6I", 1, 2, 3, 4, 12, 0x100001B)),
            },
        }

    def decode_report(
        self, incidents, build_tag="1234abcd", report_value=..., manifest_value=...
    ):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf = root / "synthetic.elf"
            elf.write_bytes(b"synthetic selected ELF identity")
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
                        "build_tag": build_tag,
                    }
                    if manifest_value is ...
                    else manifest_value
                )
            )
            report = root / "report.json"
            report.write_text(
                json.dumps(
                    {
                        "format": "zmk-watchdog-report-v1",
                        "incidents": incidents,
                    }
                    if report_value is ...
                    else report_value
                )
            )
            argv = [
                "decoder",
                str(report),
                "--elf",
                str(elf),
                "--build-manifest",
                str(manifest),
            ]
            fake = subprocess.CompletedProcess(
                [], 0, "synthetic_function\nsynthetic.c:7\n", ""
            )
            with (
                patch.object(sys, "argv", argv),
                patch.object(decoder.subprocess, "run", return_value=fake) as lookup,
                redirect_stdout(StringIO()) as output,
            ):
                decoder.main()
            return json.loads(output.getvalue())["incidents"], lookup.call_count

    def test_bad_common_capture_preserves_fatal_raw_data_and_next_record(self):
        good = bytearray(192)
        good[0] = 1
        cases = {"short": [1], "missing": [], "not_bytes": [True], "null": None}
        for label, offset, value in [
            ("format", 0, 99),
            ("flags", 1, 128),
            ("address_count", 2, 7),
            ("operation_count", 3, 5),
            ("capabilities", 92, 2),
        ]:
            malformed = good.copy()
            malformed[offset] = value
            cases[label] = list(malformed)
        for label, capture in cases.items():
            with self.subTest(case=label):
                raw = self.incident(capture)
                next_record = self.incident(list(good), incident_id=2)
                decoded, calls = self.decode_report([raw, next_record])
                first = decoded[0]
                self.assertEqual(first["raw_incident"], raw)
                self.assertEqual(first["raw_capture_context"], capture)
                self.assertEqual(first["fatal"], raw["fatal"])
                self.assertEqual(first["fatal_context"]["r12"], 12)
                self.assertEqual(
                    [s["lookup"] for s in first["symbols"]], [0x1000, 0x1FFE]
                )
                self.assertIsNone(first["capture_context"])
                self.assertTrue(first["capture_context_error"])
                self.assertEqual(first["error"], first["capture_context_error"])
                self.assertEqual(decoded[1]["id"], 2)
                self.assertNotIn("error", decoded[1])
                self.assertEqual(calls, 4)

    def test_invalid_capture_flags_are_per_record_errors(self):
        capture = [1] + [0] * 191
        for flags in [None, True, False, -1, 0x100000000, "4", 4.0]:
            with self.subTest(flags=flags):
                raw = self.incident(capture, flags)
                decoded, calls = self.decode_report(
                    [raw, self.incident(capture, incident_id=2)]
                )
                self.assertEqual(decoded[0]["raw_incident"], raw)
                self.assertIn("captureFlags", decoded[0]["capture_context_error"])
                self.assertIn("uint32", decoded[0]["capture_context_error"])
                self.assertEqual(len(decoded[0]["symbols"]), 2)
                self.assertEqual(decoded[1]["id"], 2)
                self.assertEqual(calls, 4)

    def test_valid_common_capture_keeps_raw_and_all_symbol_kinds(self):
        capture = bytearray(192)
        capture[:4] = bytes([1, 16, 2, 1])
        struct.pack_into("<2I", capture, 8, 0x3001, 7)
        struct.pack_into("<2I", capture, 64, 0x1000, 0x2001)
        struct.pack_into("<I", capture, 92, 1)
        struct.pack_into(
            "<IIIHHII", capture, 96, 1, 123, 0x4001, 42, 7, 0xFFFFFFFF, 0x80000000
        )
        raw = self.incident(list(capture))
        decoded, calls = self.decode_report([raw])
        item = decoded[0]
        self.assertEqual(item["raw_incident"], raw)
        self.assertEqual(item["raw_capture_context"], list(capture))
        self.assertNotIn("error", item)
        self.assertEqual(item["capture_context"]["operations"][0]["arg1"], 0x80000000)
        self.assertEqual(
            [s["lookup"] for s in item["symbols"]],
            [0x1000, 0x1FFE, 0x2FFE, 0x1000, 0x1FFE, 0x3FFE],
        )
        self.assertEqual(calls, 6)

    def test_bad_common_capture_does_not_discard_valid_freeze_trace(self):
        raw = self.incident([99] + [0] * 191)
        del raw["fatal"]
        raw["type"] = 0
        raw["freeze"] = {
            "queueName": "synthetic",
            "trace": list(
                bytes([1, 0, 1, 2])
                + struct.pack("<8I", 0, 0x20001000, 0x1000, 0x2001, 0, 0, 0, 0)
            ),
        }
        decoded, calls = self.decode_report([raw])
        self.assertEqual(decoded[0]["raw_incident"], raw)
        self.assertEqual(decoded[0]["trace"]["addresses"], [0x1000, 0x2001])
        self.assertEqual(len(decoded[0]["symbols"]), 2)
        self.assertIn("capture_context_error", decoded[0])
        self.assertEqual(calls, 2)

    def test_wrong_build_still_blocks_every_symbol_lookup(self):
        raw = self.incident([99] + [0] * 191)
        decoded, calls = self.decode_report([raw], build_tag="87654321")
        self.assertEqual(decoded[0]["raw_incident"], raw)
        self.assertEqual(decoded[0]["raw_capture_context"], raw["captureContext"])
        self.assertIn("build tag differs", decoded[0]["error"])
        self.assertNotIn("symbols", decoded[0])
        self.assertEqual(calls, 0)

    def test_bad_fatal_register_extension_keeps_primary_and_next_record(self):
        for context in [[1], [True] * 24, None, {}, [256] * 24]:
            with self.subTest(context=context):
                raw = self.incident([], flags=0)
                raw["fatal"]["context"] = context
                decoded, calls = self.decode_report(
                    [raw, self.incident([], flags=0, incident_id=2)]
                )
                first = decoded[0]
                self.assertEqual(first["raw_incident"], raw)
                self.assertEqual(first["fatal"], raw["fatal"])
                self.assertIsNone(first["fatal_context"])
                self.assertTrue(first["fatal_context_error"])
                self.assertEqual(
                    [s["lookup"] for s in first["symbols"]], [0x1000, 0x1FFE]
                )
                self.assertEqual(decoded[1]["id"], 2)
                self.assertNotIn("error", decoded[1])
                self.assertEqual(calls, 4)

    def test_bad_trace_keeps_independent_common_addresses(self):
        capture = bytearray(192)
        capture[:2] = bytes([1, 16])
        struct.pack_into("<I", capture, 8, 0x3001)
        for trace in [[1], [True] * 36, None, {}, [256] * 36]:
            with self.subTest(trace=trace):
                raw = self.incident(list(capture))
                del raw["fatal"]
                raw["freeze"] = {"queueName": "synthetic", "trace": trace}
                decoded, calls = self.decode_report(
                    [raw, self.incident([], flags=0, incident_id=2)]
                )
                first = decoded[0]
                self.assertEqual(first["raw_incident"], raw)
                self.assertEqual(first["raw_trace"], trace)
                self.assertIsNone(first["trace"])
                self.assertTrue(first["trace_error"])
                self.assertEqual(first["capture_context"]["assertion_site"], 0x3001)
                self.assertEqual(first["symbols"][0]["lookup"], 0x2FFE)
                self.assertEqual(calls, 3)

    def test_non_object_incident_cannot_abort_later_records(self):
        for malformed in [None, [], "bad", 7, True, False]:
            with self.subTest(incident=malformed):
                decoded, calls = self.decode_report(
                    [malformed, self.incident([], flags=0, incident_id=2)]
                )
                self.assertEqual(decoded[0]["raw_incident"], malformed)
                self.assertIn("incident must be an object", decoded[0]["error"])
                self.assertEqual(decoded[1]["id"], 2)
                self.assertNotIn("error", decoded[1])
                self.assertEqual(calls, 2)

    def test_non_object_details_are_local_errors_even_when_falsey(self):
        for field in ["fatal", "freeze"]:
            for malformed in [[], "bad", 7, True, False, 0, ""]:
                with self.subTest(field=field, value=malformed):
                    raw = self.incident([], flags=0)
                    del raw["fatal"]
                    raw[field] = malformed
                    decoded, calls = self.decode_report(
                        [raw, self.incident([], flags=0, incident_id=2)]
                    )
                    self.assertEqual(decoded[0]["raw_incident"], raw)
                    self.assertIn(f"{field} must be an object", decoded[0]["error"])
                    self.assertEqual(decoded[1]["id"], 2)
                    self.assertEqual(calls, 2)

    def test_wrong_build_blocks_all_lookups_even_with_other_bad_fields(self):
        raw = self.incident([1, 16] + [0] * 190)
        raw["fatal"]["context"] = [1]
        raw["freeze"] = "bad"
        decoded, calls = self.decode_report([raw], build_tag="87654321")
        self.assertEqual(decoded[0]["raw_incident"], raw)
        self.assertIn("build tag differs", decoded[0]["error"])
        self.assertNotIn("symbols", decoded[0])
        self.assertEqual(calls, 0)

    def test_bad_report_and_manifest_shapes_are_cli_errors(self):
        for report in [
            None,
            [],
            "bad",
            {"format": "zmk-watchdog-report-v1", "incidents": None},
            {"format": "zmk-watchdog-report-v1", "incidents": {}},
            {"format": "zmk-watchdog-report-v1", "incidents": "bad"},
        ]:
            with self.subTest(report=report), self.assertRaises(SystemExit) as error:
                self.decode_report([], report_value=report)
            self.assertEqual(error.exception.code, 2)
        for manifest in [None, [], "bad", 7]:
            with (
                self.subTest(manifest=manifest),
                self.assertRaises(SystemExit) as error,
            ):
                self.decode_report([], manifest_value=manifest)
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
