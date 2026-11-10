"""Exercise the actual owner command boundary with a controlled west process."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


class BuildBudgetContract(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        source = root / "tests/test_owner.py"
        spec = importlib.util.spec_from_file_location(
            "owner_build_budget_target", source
        )
        self.module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = self.module
        sys.path.insert(0, str(root / "tests"))
        try:
            spec.loader.exec_module(self.module)
        finally:
            sys.path.pop(0)

    def command(self, selected=None):
        environment = dict(os.environ)
        environment.pop("OWNER_TEST_BUILD_PARALLELISM", None)
        if selected is not None:
            environment["OWNER_TEST_BUILD_PARALLELISM"] = selected
        result = subprocess.CompletedProcess([], 0, "controlled west output", "")
        with (
            patch.dict(os.environ, environment, clear=True),
            patch.object(self.module.subprocess, "run", return_value=result) as child,
        ):
            self.module.run_west(["zmk-build", "--no-run"])
        return child.call_args.args[0]

    def test_default_has_one_target_with_original_arguments(self):
        self.assertEqual(
            self.command(), ["west", "zmk-build", "--parallelism", "1", "--no-run"]
        )

    def test_explicit_positive_target_budget_is_forwarded(self):
        self.assertEqual(
            self.command("2"), ["west", "zmk-build", "--parallelism", "2", "--no-run"]
        )

    def test_invalid_budget_stops_before_starting_west(self):
        for value in ("0", "-1", "unbounded"):
            with (
                self.subTest(value=value),
                patch.dict(os.environ, {"OWNER_TEST_BUILD_PARALLELISM": value}),
                patch.object(self.module.subprocess, "run") as child,
            ):
                with self.assertRaises(ValueError):
                    self.module.run_west(["zmk-build", "--no-run"])
                child.assert_not_called()

    def context_command(self, selected=None):
        environment = dict(os.environ)
        environment.pop("OWNER_TEST_BUILD_PARALLELISM", None)
        if selected is not None:
            environment["OWNER_TEST_BUILD_PARALLELISM"] = selected
        results = [
            subprocess.CompletedProcess([], 0, "/resolved/zephyr\n", ""),
            subprocess.CompletedProcess(
                [], 0, "WATCHDOG_CONTEXT_PASS variants=2 repeats=3", ""
            ),
        ]
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.dict(os.environ, environment, clear=True),
            patch.object(self.module.subprocess, "run", side_effect=results) as child,
        ):
            case = self.module.WestCommandsTests("test_common_context")
            case.BUILD_DIR = Path(directory)
            case.test_common_context()
        self.assertEqual(child.call_count, 2)
        command = child.call_args_list[1].args[0]
        self.assertEqual(command[:2], ["python3", "tests/context/run.py"])
        self.assertEqual(command.count("--jobs"), 1)
        return command[command.index("--jobs") + 1]

    def test_context_default_budget_is_forwarded(self):
        self.assertEqual(self.context_command(), "1")

    def test_context_explicit_budget_is_forwarded(self):
        for value in ("1", "2"):
            with self.subTest(value=value):
                self.assertEqual(self.context_command(value), value)

    def test_context_invalid_budget_stops_before_launch(self):
        for value in ("0", "-1", "unbounded"):
            results = [
                subprocess.CompletedProcess([], 0, "/resolved/zephyr\n", ""),
                subprocess.CompletedProcess(
                    [], 0, "WATCHDOG_CONTEXT_PASS variants=2 repeats=3", ""
                ),
            ]
            with (
                self.subTest(value=value),
                tempfile.TemporaryDirectory() as directory,
                patch.dict(os.environ, {"OWNER_TEST_BUILD_PARALLELISM": value}),
                patch.object(
                    self.module.subprocess, "run", side_effect=results
                ) as child,
            ):
                case = self.module.WestCommandsTests("test_common_context")
                case.BUILD_DIR = Path(directory)
                with self.assertRaises(ValueError):
                    case.test_common_context()
                child.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
