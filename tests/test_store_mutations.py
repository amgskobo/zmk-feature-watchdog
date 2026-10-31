"""The preserved overwrite operators fail closed on stale/ambiguous selectors."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "overwrite_controls", ROOT / "tests/store/mutate_overwrite.py"
)
CONTROLS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROLS)


class StoreMutationSelectors(unittest.TestCase):
    def test_original_ten_names_and_current_source_selection(self):
        self.assertEqual(
            [row[0] for row in CONTROLS.MUTATIONS],
            [
                "victim-is-newest",
                "victim-is-first-used",
                "ordinal-not-wrap-safe",
                "guard-removed",
                "guard-inverted",
                "no-overwrite",
                "stopped-in-status",
                "stopped-in-accessor",
                "drop-counted-on-replace",
                "stale-ordinal-on-reload",
            ],
        )
        source = (ROOT / "src/watchdog_store.c").read_bytes().decode("utf-8")
        for name, pattern, replacement in CONTROLS.MUTATIONS:
            with self.subTest(name=name):
                self.assertNotEqual(
                    CONTROLS.mutate(source, name, pattern, replacement), source
                )

    def test_missing_and_ambiguous_selection_are_rejected(self):
        source = (ROOT / "src/watchdog_store.c").read_bytes().decode("utf-8")
        for name, pattern, replacement in CONTROLS.MUTATIONS:
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "EXACTLY ONCE"):
                    CONTROLS.mutate(
                        "no selected production code", name, pattern, replacement
                    )
                with self.assertRaisesRegex(ValueError, "EXACTLY ONCE"):
                    CONTROLS.mutate(source + source, name, pattern, replacement)
