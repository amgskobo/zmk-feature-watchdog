"""Offline report contract; no firmware or transport substitutes."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout
from io import StringIO
import struct

SCRIPT = Path(__file__).resolve().parents[2] / "tools/decode_trace.py"
spec = importlib.util.spec_from_file_location("decoder", SCRIPT)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


class DecoderTests(unittest.TestCase):
    def test_first_common_candidate_can_be_a_return_after_filtered_pc(self):
        capture = bytearray(192)
        capture[:4] = bytes([1, 1, 1, 0])
        struct.pack_into("<I", capture, 64, 0x2201)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf = root / "selected.elf"
            elf.write_bytes(b"synthetic ELF identity")
            report = root / "report.json"
            report.write_text(
                json.dumps(
                    {
                        "format": "zmk-watchdog-report-v1",
                        "incidents": [
                            {
                                "fatal": {"pc": pc, "lr": 0x2201},
                                "captureFlags": 4,
                                "captureContext": list(capture),
                            }
                            for pc in [0x3000, 0x2201]
                        ],
                    }
                )
            )
            fake = subprocess.CompletedProcess(
                [], 0, "synthetic_callback\nsynthetic.c:7\n", ""
            )
            with (
                patch.object(
                    sys,
                    "argv",
                    [
                        "decoder",
                        str(report),
                        "--elf",
                        str(elf),
                        "--expected-elf-sha256",
                        hashlib.sha256(elf.read_bytes()).hexdigest(),
                    ],
                ),
                patch.object(decoder.subprocess, "run", return_value=fake),
                redirect_stdout(StringIO()) as output,
            ):
                decoder.main()
            incidents = json.loads(output.getvalue())["incidents"]
            lookups = [
                next(
                    s["lookup"]
                    for s in item["symbols"]
                    if s["kind"] == "common capture address candidate"
                )
                for item in incidents
            ]
            self.assertEqual(lookups, [0x21FE, 0x2200])

    def test_common_capture_wide_arguments_and_bounds(self):
        data = bytearray(192)
        data[:4] = bytes([1, 2 | 64, 2, 2])
        struct.pack_into("<8I", data, 32, *range(4, 12))
        struct.pack_into("<2I", data, 64, 0x1000, 0x2001)
        struct.pack_into(
            "<IIIHHII",
            data,
            96,
            1,
            0xFFFFFFF0,
            0x3001,
            0xFFFF,
            7,
            0xFFFFFFFF,
            0x80000000,
        )
        struct.pack_into("<IIIHHII", data, 120, 2, 1, 0x4001, 42, 8, 3, 4)
        result = decoder.decode_capture_context(list(data))
        self.assertIsNone(result["time_ms"])
        struct.pack_into("<I", data, 92, 1)
        self.assertEqual(decoder.decode_capture_context(list(data))["time_ms"], 0)
        self.assertEqual(result["callee"], {f"r{i}": i for i in range(4, 12)})
        self.assertEqual(result["addresses"], [0x1000, 0x2001])
        self.assertEqual([x["time_ms"] for x in result["operations"]], [0xFFFFFFF0, 1])
        self.assertEqual(result["operations"][0]["arg1"], 0x80000000)
        for index, value in [(0, 2), (1, 128), (2, 7), (3, 5)]:
            malformed = bytearray(data)
            malformed[index] = value
            with self.assertRaises(ValueError):
                decoder.decode_capture_context(list(malformed))
        self.assertIsNone(decoder.decode_capture_context([]))
        with self.assertRaises(ValueError):
            decoder.decode_capture_context([1])
        struct.pack_into("<I", data, 92, 2)
        with self.assertRaises(ValueError):
            decoder.decode_capture_context(list(data))

    def test_signed_persistence_and_audit_results(self):
        raw = bytearray(80)
        raw[:2] = bytes([1, 2])
        struct.pack_into("<IBBH", raw, 24, 99, 21, 255, 65531)
        struct.pack_into("<IBBH", raw, 32, 100, 22, 255, 65524)
        decoded = decoder.decode_diagnostics(list(raw))
        self.assertEqual(
            [(e["time_ms"], e["result"]) for e in decoded["events"]],
            [(99, -5), (100, -12)],
        )

    def test_runtime_checkpoint_is_separate_from_incident_identity(self):
        raw = bytearray(80)
        raw[:2] = bytes([1, 1])
        struct.pack_into("<I", raw, 4, 0x12345678)
        struct.pack_into("<IBBH", raw, 24, 0xFFFFFFF0, 20, 0, 65524)
        status = dict(
            buildTag=0x87654321,
            runtimeFlags=1 | 128,
            resetCause=1,
            previousDiagnostics=list(raw),
        )
        result = decoder.decode_runtime(status)
        self.assertEqual(result["running_build_tag"], "87654321")
        self.assertEqual(result["previous_diagnostics"]["build_tag"], "12345678")
        self.assertEqual(result["previous_diagnostics"]["events"][0]["result"], -12)
        self.assertEqual(result["raw_status"], status)
        self.assertIsNone(decoder.decode_runtime({"capacity": 16}))
        status["previousDiagnostics"] = [255]
        self.assertIn(
            "unknown diagnostic format", decoder.decode_runtime(status)["error"]
        )
        self.assertEqual(
            decoder.decode_runtime(status)["raw_status"]["previousDiagnostics"], [255]
        )
        for flags in ["invalid", True, -1, 0x100000000]:
            self.assertIn(
                "invalid runtime",
                decoder.decode_runtime({"runtimeFlags": flags})["error"],
            )

    def test_recent_events_and_fatal_pc_lr_are_kept(self):
        raw = bytearray(80)
        raw[:4] = bytes([1, 2, 3, 1])
        struct.pack_into("<I", raw, 4, 0x12345678)
        struct.pack_into("<IBBH", raw, 24, 0xFFFFFFF0, 10, 3, 0)
        struct.pack_into("<IBBH", raw, 32, 1, 15, 3, 65514)
        diagnostic = decoder.decode_diagnostics(list(raw))
        self.assertEqual(
            [(e["time_ms"], e["name"], e["result"]) for e in diagnostic["events"]],
            [(0xFFFFFFF0, "BT_CLR begin", 0), (1, "disconnect returned", -22)],
        )
        self.assertEqual(
            decoder.decode_fatal_context(
                list(struct.pack("<6I", 3, 2, 1, 0, 12, 0x1000011))
            )["exception_number"],
            17,
        )
        for invalid in [[], [1] * 23, [True] * 24]:
            if invalid:
                with self.assertRaises(ValueError):
                    decoder.decode_fatal_context(invalid)
            else:
                self.assertIsNone(decoder.decode_fatal_context(invalid))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf = root / "zmk.elf"
            elf.write_bytes(b"fixture")
            report = root / "report.json"
            report.write_text(
                json.dumps(
                    {
                        "format": "zmk-watchdog-report-v1",
                        "incidents": [
                            {
                                "id": 5,
                                "fatal": {
                                    "reason": 3,
                                    "pc": 343390,
                                    "lr": 343377,
                                    "threadName": "?",
                                },
                                "diagnostics": list(raw),
                            },
                            {
                                "id": 6,
                                "fatal": {
                                    "reason": 3,
                                    "pc": 337522,
                                    "lr": 337507,
                                    "threadName": "?",
                                },
                            },
                        ],
                    }
                )
            )
            fake = subprocess.CompletedProcess([], 0, stdout="function\nsource.c:906\n")
            with (
                patch.object(sys, "argv", ["decoder", str(report), "--elf", str(elf)]),
                patch.object(decoder.subprocess, "run", return_value=fake) as lookup,
                redirect_stdout(StringIO()) as output,
            ):
                decoder.main()
            decoded = json.loads(output.getvalue())
            self.assertEqual([i["id"] for i in decoded["incidents"]], [5, 6])
            self.assertEqual(
                [i["symbols"][0]["lookup"] for i in decoded["incidents"]],
                [0x53D5E, 0x52672],
            )
            self.assertEqual(decoded["incidents"][0]["symbols"][1]["lookup"], 0x53D4E)
            self.assertEqual(lookup.call_count, 4)
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
                        "build_tag": "87654321",
                    }
                )
            )
            # Wrong per-incident tag prevents all symbol lookups for that incident;
            # untagged legacy records retain the explicit selected-ELF behavior.
            with (
                patch.object(
                    sys,
                    "argv",
                    [
                        "decoder",
                        str(report),
                        "--elf",
                        str(elf),
                        "--build-manifest",
                        str(manifest),
                    ],
                ),
                patch.object(decoder.subprocess, "run", return_value=fake) as lookup,
                redirect_stdout(StringIO()) as output,
            ):
                decoder.main()
            self.assertIn(
                "build tag differs",
                json.loads(output.getvalue())["incidents"][0]["error"],
            )
            self.assertEqual(lookup.call_count, 2)

    def test_independent_wire_and_invalid_formats(self):
        wire = [
            1,
            2,
            5,
            2,
            0x24,
            1,
            0,
            0x20,
            0,
            0x10,
            0,
            0x20,
            0x20,
            0x10,
            0,
            0,
            0x31,
            0x20,
            0,
            0x80,
        ] + [0] * 16
        trace = decoder.decode_trace(wire)
        self.assertEqual(trace["addresses"], [0x1020, 0x80002031])
        self.assertEqual(trace["wait_queue"], 0x20000124)
        self.assertEqual(trace["stack_pointer"], 0x20001000)
        self.assertIsNone(decoder.decode_trace([]))
        for offset, value in [
            (0, 2),
            (2, 32),
            (2, 4),
            (3, 7),
            (4, -1),
            (4, 256),
            (4, True),
        ]:
            bad = wire.copy()
            bad[offset] = value
            with self.assertRaises(ValueError):
                decoder.decode_trace(bad)
        for length in range(1, 36):
            with self.assertRaises(ValueError):
                decoder.decode_trace(wire[:length])

    def test_cli_keeps_order_and_rejects_wrong_elf_before_symbol_lookup(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            elf = root / "selected.elf"
            elf.write_bytes(b"identity fixture")
            report = root / "report.json"
            report.write_text(
                json.dumps(
                    {
                        "format": "zmk-watchdog-report-v1",
                        "incidents": [
                            {
                                "id": 7,
                                "uptimeS": 10,
                                "freeze": {"queueName": "sysworkq", "trace": []},
                            },
                            {
                                "id": 1,
                                "uptimeS": 975,
                                "freeze": {"queueName": "sysworkq", "trace": [255]},
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            args = [
                sys.executable,
                str(SCRIPT),
                str(report),
                "--elf",
                str(elf),
                "--addr2line",
                "intentionally-absent-must-not-run",
            ]
            good = subprocess.run(
                args
                + [
                    "--expected-elf-sha256",
                    hashlib.sha256(elf.read_bytes()).hexdigest(),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(good.returncode, 0, good.stderr)
            decoded = json.loads(good.stdout)
            self.assertEqual([i["id"] for i in decoded["incidents"]], [7, 1])
            self.assertIsNone(decoded["incidents"][0]["trace"])
            self.assertEqual(decoded["incidents"][1]["raw_trace"], [255])
            self.assertIn("error", decoded["incidents"][1])
            bad = subprocess.run(
                args + ["--expected-elf-sha256", "0" * 64],
                capture_output=True,
                text=True,
            )
            self.assertEqual(bad.returncode, 2)
            self.assertIn("ELF SHA-256 mismatch", bad.stderr)


if __name__ == "__main__":
    unittest.main()
