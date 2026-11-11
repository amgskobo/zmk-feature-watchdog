"""Selected test units remain bound to the diagnostic source manifest after relocation."""

import importlib.util
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "watchdog_build_identity", ROOT / "tools/build_identity.py"
)
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class BuildIdentityTests(unittest.TestCase):
    def test_native_fixture_inputs_are_selected_and_unselected_metadata_is_excluded(
        self,
    ):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for directory in [
                "src",
                "include",
                "proto",
                "tools",
                "tests/native-input",
                "tests/support",
                "dts",
            ]:
                shutil.copytree(ROOT / directory, root / directory)
            for name in ["Kconfig", "CMakeLists.txt"]:
                shutil.copyfile(ROOT / name, root / name)
            for name in ["test", "watchdog"]:
                shutil.copytree(ROOT / "tests" / name, root / "tests" / name)
            config = root / ".config"
            config.write_text(
                "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y\n"
                "CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y\n"
            )

            def identity_for(name, selected):
                output = root / name
                args = [
                    "build_identity.py",
                    "--root",
                    str(root),
                    "--config",
                    str(config),
                    "--zephyr",
                    str(root),
                    "--zmk",
                    str(root),
                    "--output",
                    str(output),
                ]
                if selected:
                    args += ["--native-input-dir", str(root / "tests/test")]
                with patch("sys.argv", args):
                    identity.main()
                return json.loads((output / "watchdog-build-identity.json").read_text())

            selected = identity_for("selected", True)
            for name in [
                "tests/native-input/kscan.c",
                "tests/native-input/word.h",
                "tests/native-input/CMakeLists.txt",
                "tests/native-input/sanitizers.cmake",
                "tests/test/native_sim.keymap",
                "tests/test/native_sim.conf",
                "tests/test/extra-cmake-args",
                "dts/bindings/kscan/amgskobo,watchdog-test-kscan.yaml",
            ]:
                self.assertEqual(
                    selected["inputs"][name],
                    hashlib.sha256((root / name).read_bytes()).hexdigest(),
                )
            self.assertNotIn("tests/watchdog/native_sim.conf", selected["inputs"])
            config.write_text("")
            unselected = identity_for("unselected", False)
            with (root / "tests/native-input/word.h").open("a") as stream:
                stream.write("\n/* synthetic unselected fixture change */\n")
            after = identity_for("unselected-after", False)
            self.assertEqual(
                unselected["source_config_sha256"], after["source_config_sha256"]
            )
            self.assertNotIn("tests/native-input/word.h", unselected["inputs"])
            self.assertNotIn(
                "tests/native-input/sanitizers.cmake", unselected["inputs"]
            )
            self.assertNotEqual(
                unselected["configuration_nonce"], after["configuration_nonce"]
            )

    def test_native_word_header_reconfigures_actual_identity_cmake(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            owner = root / "owner"
            owner.mkdir()
            for directory in [
                "src",
                "include",
                "proto",
                "tools",
                "tests/native-input",
                "tests/support",
                "tests/test",
                "dts",
            ]:
                shutil.copytree(ROOT / directory, owner / directory)
            for name in ["CMakeLists.txt", "Kconfig"]:
                shutil.copyfile(ROOT / name, owner / name)
            # Model Zephyr's existing raw quoted-include dependency. The owner
            # must not add a second normalized alias to Ninja's phony inputs.
            (owner / "tests/shared.dtsi").write_text("/* synthetic quoted include */\n")
            with (owner / "tests/test/native_sim.keymap").open("a") as stream:
                stream.write('\n#include "../shared.dtsi"\n')
            (root / "noop.c").write_text("int identity_probe_placeholder;\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(input_identity C)\n"
                "find_package(Python3 REQUIRED COMPONENTS Interpreter)\n"
                "set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})\n"
                "set(ZEPHYR_BASE ${CMAKE_CURRENT_SOURCE_DIR})\n"
                "set(APPLICATION_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})\n"
                "set(CONFIG_ZMK_WATCHDOG ON)\nset(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS ON)\n"
                "set(CONFIG_BOARD_NATIVE_SIM ON)\nset(CONFIG_ASAN ON)\n"
                "set(CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE ON)\n"
                "set(CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE ON)\n"
                "set(ZMK_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/owner/tests/test)\n"
                'file(WRITE ${PROJECT_BINARY_DIR}/.config "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y\\nCONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y\\n")\n'
                "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ZMK_CONFIG}/../shared.dtsi)\n"
                "function(zephyr_include_directories)\nendfunction()\n"
                "add_library(app STATIC noop.c)\nadd_library(native_simulator INTERFACE)\nadd_subdirectory(owner)\n"
                "add_custom_target(identity_probe COMMAND ${CMAKE_COMMAND} -E echo identity_probe)\n"
            )
            build = root / "build"
            result = subprocess.run(
                ["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = build / "owner/diagnostics/watchdog-build-identity.json"
            before = json.loads(manifest.read_text())
            with (owner / "tests/native-input/word.h").open("a") as stream:
                stream.write("\n/* synthetic selected header change */\n")
            result = subprocess.run(
                ["cmake", "--build", str(build), "--target", "identity_probe"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            after = json.loads(manifest.read_text())
            self.assertNotEqual(
                before["source_config_sha256"], after["source_config_sha256"]
            )
            self.assertNotEqual(
                before["inputs"]["tests/native-input/word.h"],
                after["inputs"]["tests/native-input/word.h"],
            )

    def test_only_selected_test_sources_and_their_shared_header_are_bound(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            paths = [
                "src/watchdog.c",
                "include/watchdog.h",
                "Kconfig",
                "CMakeLists.txt",
                "tests/ztest/watchdog_test.c",
                "tests/ztest/watchdog_rpc_test.c",
                "tests/support/watchdog_relay_assertions_0.inc",
                "tests/support/watchdog_relay_assertions_1.inc",
                "tests/support/init_result.h",
                "tests/support/context_relay_assertions.h",
            ]
            for relative in paths:
                p = root / relative
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(relative)
            scenarios = [
                ("", set()),
                ("CONFIG_ZMK_WATCHDOG_TEST=y\n", {paths[4]}),
                ("CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST=y\n", {paths[5]}),
                ("CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y\n", {paths[6], paths[9]}),
                (
                    "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y\nCONFIG_ZMK_SPLIT_ROLE_CENTRAL=y\n",
                    {paths[7]},
                ),
            ]
            for index, (config, expected) in enumerate(scenarios):
                with self.subTest(config=config):
                    (root / ".config").write_text(config)
                    output = root / str(index)
                    args = [
                        "build_identity.py",
                        "--root",
                        str(root),
                        "--config",
                        str(root / ".config"),
                        "--zephyr",
                        str(root),
                        "--zmk",
                        str(root),
                        "--output",
                        str(output),
                    ]
                    with (
                        patch("sys.argv", args),
                        patch.object(
                            identity.subprocess,
                            "check_output",
                            return_value="revision\n",
                        ),
                        patch.object(
                            identity.secrets,
                            "token_hex",
                            return_value="fixed-test-nonce",
                        ),
                    ):
                        identity.main()
                    manifest = json.loads(
                        (output / "watchdog-build-identity.json").read_text()
                    )
                    selected = {
                        name for name in manifest["inputs"] if name.startswith("tests/")
                    }
                    self.assertEqual(
                        selected, expected | ({paths[8]} if expected else set())
                    )
                    self.assertIn("src/watchdog.c", manifest["inputs"])
                    self.assertIn(".config", manifest["inputs"])

    def test_peripheral_context_header_reconfigures_and_changes_identity(self):
        # Evaluate the owner's actual identity CMake path without compiling an
        # application. Firmware/Kconfig acceptance remains in the owner matrix.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            owner = root / "owner"
            owner.mkdir()
            for directory in ["src", "include", "proto", "tools", "tests/support"]:
                shutil.copytree(ROOT / directory, owner / directory)
            shutil.copyfile(ROOT / "CMakeLists.txt", owner / "CMakeLists.txt")
            shutil.copyfile(ROOT / "Kconfig", owner / "Kconfig")
            (root / "noop.c").write_text("int identity_probe_placeholder;\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(identity_dependency_probe C)\n"
                "find_package(Python3 REQUIRED COMPONENTS Interpreter)\n"
                "set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})\n"
                "set(ZEPHYR_BASE ${CMAKE_CURRENT_SOURCE_DIR})\n"
                "set(APPLICATION_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})\n"
                "set(CONFIG_ZMK_WATCHDOG ON)\n"
                "set(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS ON)\n"
                "set(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST ON)\n"
                "file(WRITE ${PROJECT_BINARY_DIR}/.config "
                '"CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y\\n")\n'
                "function(zephyr_include_directories)\nendfunction()\n"
                "add_library(app STATIC noop.c)\n"
                "add_subdirectory(owner)\n"
                "add_custom_target(identity_probe "
                "COMMAND ${CMAKE_COMMAND} -E echo identity_dependency_probe)\n"
            )
            build = root / "build"
            configured = subprocess.run(
                ["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                configured.returncode, 0, configured.stdout + configured.stderr
            )
            manifest = build / "owner/diagnostics/watchdog-build-identity.json"
            before = json.loads(manifest.read_text())
            name = "tests/support/context_relay_assertions.h"
            with (owner / name).open("a") as stream:
                stream.write("\n/* synthetic identity dependency change */\n")
            rebuilt = subprocess.run(
                ["cmake", "--build", str(build), "--target", "identity_probe"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
            after = json.loads(manifest.read_text())
            self.assertNotEqual(
                before["source_config_sha256"], after["source_config_sha256"]
            )
            self.assertNotEqual(before["inputs"].get(name), after["inputs"].get(name))
            self.assertEqual(
                after["inputs"][name],
                hashlib.sha256((owner / name).read_bytes()).hexdigest(),
            )

    def test_unreadable_checkout_records_unknown_revision(self):
        failures = [
            FileNotFoundError("git"),
            identity.subprocess.CalledProcessError(128, "git"),
        ]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                with tempfile.TemporaryDirectory() as folder:
                    root = Path(folder)
                    for relative in ["src/watchdog.c", "Kconfig", "CMakeLists.txt"]:
                        p = root / relative
                        p.parent.mkdir(parents=True, exist_ok=True)
                        p.write_text(relative)
                    (root / ".config").write_text("")
                    args = ["build_identity.py", "--root", str(root)]
                    args += ["--config", str(root / ".config")]
                    args += ["--zephyr", str(root), "--zmk", str(root)]
                    args += ["--output", str(root / "out")]
                    with (
                        patch("sys.argv", args),
                        patch.object(
                            identity.subprocess, "check_output", side_effect=failure
                        ),
                    ):
                        identity.main()
                    manifest = json.loads(
                        (root / "out" / "watchdog-build-identity.json").read_text()
                    )
                    self.assertEqual(manifest["inputs"]["zephyr_revision"], "unknown")
                    self.assertEqual(manifest["inputs"]["zmk_revision"], "unknown")
                    self.assertTrue((root / "out" / "watchdog_build_id.h").read_text())


if __name__ == "__main__":
    unittest.main()
