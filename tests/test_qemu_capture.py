"""Reject every truncated UART terminal prefix, including an EOF suffix loss."""

import unittest
from unittest.mock import Mock

from support.qemu_capture import complete_terminal_record, wait_for_terminal_record


class QemuCapture(unittest.TestCase):
    def test_every_truncated_prefix(self):
        for record in ("RESULT: PASS failures=0\n", "RESULT: PASS failures=0\r\n"):
            for length in range(len(record)):
                with self.subTest(record=repr(record), length=length):
                    self.assertFalse(
                        complete_terminal_record("earlier log\n" + record[:length])
                    )
            self.assertTrue(complete_terminal_record("earlier log\n" + record))
        self.assertFalse(complete_terminal_record("ordinary RESULT: PASS failures=0\n"))

    def test_delayed_suffix_remains_ordered(self):
        chunks = [
            "earlier\nRESULT:",
            "earlier\nRESULT: PASS failures=0",
            "earlier\nRESULT: PASS failures=0\n",
        ]
        read = Mock(side_effect=chunks)
        process = Mock()
        process.poll.return_value = None
        pause = Mock()
        clock = Mock(side_effect=[0, 1, 2, 3])
        self.assertTrue(
            wait_for_terminal_record(process, read, clock=clock, pause=pause)
        )
        self.assertEqual(read.call_count, 3)
        self.assertEqual(pause.call_count, 2)
        self.assertEqual(process.poll.call_count, 2)
        self.assertEqual(chunks[-1], "earlier\nRESULT: PASS failures=0\n")

    def test_eof_without_newline_is_incomplete(self):
        process = Mock()
        process.poll.return_value = 0
        self.assertFalse(
            wait_for_terminal_record(
                process,
                lambda: "RESULT: PASS failures=0",
                clock=Mock(side_effect=[0, 1]),
            )
        )

    def test_complete_failure_is_captured_without_accepting_it(self):
        text = "RESULT: FAIL failures=1\n"
        self.assertTrue(complete_terminal_record(text))
        self.assertNotIn("RESULT: PASS failures=0", text)
        process = Mock()
        process.poll.return_value = 1
        self.assertFalse(
            wait_for_terminal_record(
                process,
                lambda: "FATAL ERROR 2\n",
                clock=Mock(side_effect=[0, 1]),
            )
        )

    def test_timeout_preserves_incomplete_result(self):
        text = "earlier\nRESULT: PAS"
        process = Mock()
        process.poll.return_value = None
        self.assertFalse(
            wait_for_terminal_record(
                process,
                lambda: text,
                timeout=2,
                clock=Mock(side_effect=[0, 0, 1, 2]),
                pause=Mock(),
            )
        )
        self.assertEqual(text, "earlier\nRESULT: PAS")
