import unittest

from static_memory import (
    append_stack_usage,
    capture_notification_usage,
    central_static_usage,
)


class CentralStaticMemory(unittest.TestCase):
    def test_capture_notification_storage_counts_old_and_shared_records(self):
        original = "20000000 000005d4 b incident_notification\n20001000 00000150 b fatal_record\n20002000 00000150 b freeze_record\n"
        self.assertEqual(
            capture_notification_usage(original)["capture_record_bytes"], 672
        )
        self.assertEqual(
            capture_notification_usage(original)["incident_notification_bytes"], 1492
        )
        current = "20000000 00000178 b incident_notification\n20001000 00000150 b capture_record\n"
        self.assertEqual(
            capture_notification_usage(current)["capture_record_bytes"], 336
        )
        for bad in [
            "",
            current + current,
            current.replace(" b ", " r "),
            current.splitlines()[0],
        ]:
            with self.subTest(text=bad), self.assertRaises(AssertionError):
                capture_notification_usage(bad)

    def test_append_stack_requires_one_static_positive_frame(self):
        row = "owner/src/watchdog_store.c:1:5:zmk_watchdog_store_append\t112\tstatic\n"
        self.assertEqual(append_stack_usage(row), 112)
        for bad in [
            "",
            row + row,
            row.replace("static", "dynamic,bounded"),
            row.replace("112", "0"),
        ]:
            with self.subTest(text=bad), self.assertRaises(AssertionError):
                append_stack_usage(bad)

    def test_wrapped_and_same_line_sections_exclude_rom_and_discarded(self):
        text = """Memory Configuration
RAM 0x20000000 0x40000 xrw
 .bss.first
  0x20000000 0x20 app/libapp.a(watchdog_relay_central.c.obj)
 .noinit.fifo 0x20000020 0x10 app/libapp.a(watchdog_relay_central.c.obj)
 .data.other 0x20000030 0x80 app/libapp.a(other.c.obj)
 .rom.descriptor 0x10000 0x40 app/libapp.a(watchdog_relay_central.c.obj)
 .bss.discarded 0x0 0x1000 app/libapp.a(watchdog_relay_central.c.obj)
"""
        usage = central_static_usage(text)
        self.assertEqual(usage["static_ram_bytes"], 48)
        self.assertEqual(len(usage["sections"]), 2)

    def test_missing_region_or_object_cannot_pass(self):
        for text in ["", "RAM 0x20000000 0x40000 xrw\n"]:
            with self.subTest(text=text), self.assertRaises(AssertionError):
                central_static_usage(text)

    def test_duplicate_or_out_of_region_rows_cannot_pass(self):
        prefix = "RAM 0x20000000 0x100 xrw\n"
        first = " .bss.a 0x20000000 0x20 app/libapp.a(watchdog_relay_central.c.obj)\n"
        for extra in [
            first,
            " .bss.b 0x200000f0 0x20 app/libapp.a(watchdog_relay_central.c.obj)\n",
        ]:
            with self.subTest(extra=extra), self.assertRaises(AssertionError):
                central_static_usage(prefix + first + extra)
