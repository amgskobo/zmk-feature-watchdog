from __future__ import annotations

import os
import platform
import tempfile
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

from native_verifier import verify_native
from test_native_verifier import NativeVerifierContract as NativeVerifierContract
from test_timeout_width import TimeoutWidth as TimeoutWidth
from test_qemu_capture import QemuCapture as QemuCapture
from test_store_mutations import StoreMutationSelectors as StoreMutationSelectors

from test_build_budget import BuildBudgetContract as BuildBudgetContract
from test_native_input import NativeInputContract as NativeInputContract
from decoder.test_build_identity import BuildIdentityTests as BuildIdentityTests
from test_static_memory import CentralStaticMemory as CentralStaticMemory
from static_memory import central_artifact_usage, WATCHDOG_CENTRAL_STATIC_RAM_MAX

THIS_DIR = Path(__file__).resolve().parent.parent
TEST_BUILD_DIR_NAME = "tests-zmk-feature-watchdog"


def native_artifacts_env(name: str, owner_output: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["WATCHDOG_NATIVE_ARTIFACTS"] = str(
        Path(env.get("WATCHDOG_NATIVE_ARTIFACTS", str(owner_output))) / name
    )
    return env


def build_parallelism() -> int:
    parallelism = int(os.environ.get("OWNER_TEST_BUILD_PARALLELISM", "1"))
    if parallelism < 1:
        raise ValueError("OWNER_TEST_BUILD_PARALLELISM must be a positive integer")
    return parallelism


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    if args and args[0] == "zmk-build":
        args = [args[0], "--parallelism", str(build_parallelism()), *args[1:]]
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        requested_output = os.environ.get("OWNER_TEST_OUTPUT")
        if requested_output:
            cls.BUILD_DIR = Path(requested_output).resolve()
            cls.BUILD_DIR.mkdir(parents=True, exist_ok=False)
        else:
            cls.BUILD_DIR = Path(tempfile.mkdtemp(prefix=THIS_DIR.name + "-tests-"))
        print(f"Owner test output: {cls.BUILD_DIR}", flush=True)

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / TEST_BUILD_DIR_NAME

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verify_native(test_build_dir, THIS_DIR / "tests")
        self.assertIn("PASS: test", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertIn(
            "PASS: studio_unsecured", result.stdout, result.stdout + result.stderr
        )
        self.assertIn("PASS: watchdog", result.stdout, result.stdout + result.stderr)
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)
        for variant, unsecured in [("studio", 0), ("studio_unsecured", 1)]:
            studio_log = (
                test_build_dir / f"tests/{variant}/keycode_events_full.log"
            ).read_text()
            for marker in [
                f"watchdog_notification_authorization unsecured={unsecured}",
                "watchdog_rpc_bounded_names_and_minimal_flags",
                "watchdog_rpc_get_status_empty_store",
                "watchdog_runtime_status_relay_bound",
                "watchdog_rpc_list_incidents_empty_store",
                "watchdog_rpc_list_incidents_pagination",
                "watchdog_rpc_delete_incidents_by_id",
                "watchdog_rpc_delete_incidents_all",
                "watchdog_rpc_status_recording_stopped",
                "watchdog_rpc_inject_freeze_ack",
                "watchdog_rpc_inject_freeze_recorded",
            ]:
                self.assertIn("PASS: " + marker, studio_log)

    @unittest.skipUnless(platform.system() == "Linux", "native controls require Linux")
    def test_original_mock_expected_rejections(self):
        result = subprocess.run(
            [
                "python3",
                "tests/native-input/run_controls.py",
                "--workspace",
                str(self.WEST_TOPDIR),
                "--output",
                str(self.BUILD_DIR / "original-core-mock"),
            ],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        (self.BUILD_DIR / "test_original_mock_expected_rejections.log").write_text(
            result.stdout + result.stderr
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "EXPECTED_CORE_MOCK_REJECTIONS fixtures=5 repeats=3 qualified=15",
            result.stdout,
        )

    @unittest.skipUnless(
        platform.system() == "Linux", "native kernel checks require Linux"
    )
    def test_store_concurrency(self):
        result = subprocess.run(
            ["bash", "tests/concurrency/run.sh"],
            env=native_artifacts_env("concurrency", self.BUILD_DIR),
            capture_output=True,
            text=True,
            cwd=THIS_DIR,
        )
        (self.BUILD_DIR / "test_store_concurrency.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("PASS watchdog concurrency groups=12"), 3)

    def test_relay_central(self):
        result = subprocess.run(
            [
                "python3",
                "tests/relay-central/run.py",
                "--workspace",
                str(self.WEST_TOPDIR),
                "--output",
                str(self.BUILD_DIR / "relay-central"),
            ],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        (self.BUILD_DIR / "test_relay_central.log").write_text(
            result.stdout + result.stderr
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count(": PASS"), 6)

    def test_common_context(self):
        jobs = build_parallelism()
        zephyr = run_west(["list", "zephyr", "-f", "{abspath}"])
        self.assertEqual(zephyr.returncode, 0, zephyr.stderr)
        result = subprocess.run(
            [
                "python3",
                "tests/context/run.py",
                "--zephyr",
                zephyr.stdout.strip(),
                "--output",
                str(self.BUILD_DIR / "common-context"),
                "--jobs",
                str(jobs),
            ],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        (self.BUILD_DIR / "test_common_context.log").write_text(
            result.stdout + result.stderr
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WATCHDOG_CONTEXT_PASS variants=2 repeats=3", result.stdout)

    def test_cortex_context(self):
        for name, options in [("oops", []), ("assert", ["--assert-context"])]:
            result = subprocess.run(
                [
                    "python3",
                    "tests/trace/run.py",
                    "--workspace",
                    str(self.WEST_TOPDIR),
                    "--artifacts",
                    str(self.BUILD_DIR / ("cortex-" + name)),
                    *options,
                ],
                cwd=THIS_DIR,
                capture_output=True,
                text=True,
            )
            (self.BUILD_DIR / ("test_cortex_" + name + ".log")).write_text(
                result.stdout + result.stderr
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(
        platform.system() == "Linux", "real kernel arming requires Linux"
    )
    def test_preblocked_arming(self):
        result = subprocess.run(
            ["bash", "tests/arming/run.sh"],
            env=native_artifacts_env("arming", self.BUILD_DIR),
            capture_output=True,
            text=True,
            cwd=THIS_DIR,
        )
        (self.BUILD_DIR / "test_preblocked_arming.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            result.stdout.count("PASS watchdog arms despite preblocked sysworkq"), 3
        )

    @unittest.skipUnless(
        platform.system() == "Linux", "Cortex-M stack checks require Linux/QEMU"
    )
    def test_relay_stack(self):
        command = ["python3", "tests/relay-stack/run.py"]
        for name in ("zephyr", "cmsis", "nanopb", "zmk"):
            path = run_west(["list", name, "-f", "{abspath}"])
            self.assertEqual(path.returncode, 0, path.stderr)
            command += ["--" + name, path.stdout.strip()]
        command += ["--artifacts", str(self.BUILD_DIR / "relay-stack")]
        result = subprocess.run(command, cwd=THIS_DIR, capture_output=True, text=True)
        (self.BUILD_DIR / "test_relay_stack.log").write_text(
            result.stdout + result.stderr, encoding="utf8"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(
        platform.system() == "Linux", "real kernel fairness requires Linux/QEMU"
    )
    def test_relay_fairness(self):
        command = ["python3", "tests/relay-stack/run.py", "--fairness"]
        for name in ("zephyr", "cmsis", "nanopb", "zmk"):
            path = run_west(["list", name, "-f", "{abspath}"])
            self.assertEqual(path.returncode, 0, path.stderr)
            command += ["--" + name, path.stdout.strip()]
        command += ["--artifacts", str(self.BUILD_DIR / "relay-fairness")]
        result = subprocess.run(command, cwd=THIS_DIR, capture_output=True, text=True)
        (self.BUILD_DIR / "test_relay_fairness.log").write_text(
            result.stdout + result.stderr, encoding="utf8"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("FAIRNESSCASE replenished"), 3)
        self.assertEqual(result.stdout.count("FAIRNESSCASE real-stall"), 3)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "module_watchdog_board_feature_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        "CONFIG_ZMK_USB=y",
                        "CONFIG_ZMK_BLE=y",
                        "# CONFIG_ZMK_WATCHDOG is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                    ],
                ),
                "module_watchdog_task_wdt_external": ConfigAndDeviceTree(
                    config=[
                        "# CONFIG_ZMK_WATCHDOG is not set",
                        "CONFIG_TASK_WDT=y",
                        "CONFIG_TASK_WDT_HW_FALLBACK=y",
                        "CONFIG_TASK_WDT_MIN_TIMEOUT=100",
                    ],
                    device=[],
                ),
                "module_watchdog_freeze_disabled": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "# CONFIG_ZMK_WATCHDOG_FREEZE_DETECT is not set",
                        "CONFIG_TASK_WDT=y",
                        "CONFIG_TASK_WDT_HW_FALLBACK=y",
                        "CONFIG_TASK_WDT_MIN_TIMEOUT=100",
                    ],
                    device=[],
                ),
                "module_watchdog_explicit_fallback": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_TASK_WDT_HW_FALLBACK=y",
                        "CONFIG_TASK_WDT_MIN_TIMEOUT=100",
                    ],
                    device=[],
                ),
                "module_watchdog_board_with_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        # Detectors (Phase C): default y whenever ZMK_WATCHDOG
                        # is on, on a real (non-native_sim) board.
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT=y",
                        "CONFIG_EXTRA_EXCEPTION_INFO=y",
                        "CONFIG_TASK_WDT=y",
                        # Keep the finite timer path, with no 100-ms wake.
                        "CONFIG_TASK_WDT_HW_FALLBACK=y",
                        "CONFIG_TASK_WDT_MIN_TIMEOUT=10000",
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=64",
                    ],
                    device=[],
                ),
                "module_watchdog_board_without_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_WATCHDOG_STUDIO_RPC"),
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        # Test injection is dangerous and must default off,
                        # even on a build that otherwise enables every other
                        # detector.
                        NotFound("CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y"),
                    ],
                    device=[],
                ),
                # Dedicated test-injection build: the
                # only artifact with CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y,
                # proving the InjectTest RPC wiring
                # (src/studio/watchdog_request_exec.c) compiles for a real
                # board, not just native_sim.
                "module_watchdog_board_test_injection": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_FATAL_DETECT=y",
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y",
                        # Boot-delay auto-trigger Kconfig options exist but
                        # default to 0 (disabled) even on this artifact -- see
                        # the build.yaml comment for why they aren't baked
                        # into a standing build-test artifact. (Int Kconfigs
                        # always render with their value in .config, never
                        # "is not set" -- unlike bool/tristate.)
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECT_FREEZE_AT_BOOT_MS=0",
                        "CONFIG_ZMK_WATCHDOG_TEST_INJECT_FATAL_AT_BOOT_MS=0",
                    ],
                    device=[],
                ),
                "custom_settings_board": ConfigAndDeviceTree(
                    config=[
                        # Verify that zmk-feature-custom-settings is present and enabled
                        "zmk-feature-custom-settings",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048",
                    ],
                    device=[],
                ),
                # Split keyboard build coverage. Peripheral
                # role: no Studio (ZMK_STUDIO only selects ZMK_STUDIO_RPC for
                # !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL), but the relay bridge
                # (peripheral responder side) + watchdog core still compile
                # and CONFIG_ZMK_WATCHDOG_PROTOBUF pulls in nanopb on its own.
                "module_watchdog_split_peripheral": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_SPLIT=y",
                        NotFound("CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y"),
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
                        "CONFIG_ZMK_WATCHDOG_PROTOBUF=y",
                        NotFound("CONFIG_ZMK_STUDIO=y"),
                        NotFound("CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y"),
                        # The split relay event payload stays at its
                        # framework default (DESIGN.md, Studio RPC, split
                        # relay and client).
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128",
                    ],
                    device=[],
                ),
                # Central role: local Studio RPC + the relay bridge dispatch
                # side.
                "module_watchdog_split_central": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        # Keep both buffers at the framework defaults
                        # (DESIGN.md, Studio RPC, split relay and client).
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=64",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128",
                        # The split-central fixture monitors sysworkq only;
                        # the retired feed divisor must not come back.
                        "# CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_LOWPRIO_QUEUE is not set",
                        NotFound("CONFIG_ZMK_WATCHDOG_FREEZE_FEED_DIVISOR"),
                    ],
                    device=[],
                ),
                "module_watchdog_split_central_memory": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_WATCHDOG=y",
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
                        "# CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST is not set",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_WATCHDOG_STUDIO_RPC=y",
                        "CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS=16",
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=64",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128",
                        NotFound("CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y"),
                    ],
                    device=[],
                ),
            }
        )

    def test_task_wdt_defaults(self):
        result = subprocess.run(
            [
                "python3",
                "tests/task_wdt_defaults.py",
                "--workspace",
                str(self.WEST_TOPDIR),
                "--output",
                str(self.BUILD_DIR / "task-wdt-defaults"),
            ],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        (self.BUILD_DIR / "test_task_wdt_defaults.log").write_text(
            result.stdout + result.stderr
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "nine actual Kconfig cases; old-default control rejected", result.stdout
        )

    def test_task_wdt_idle(self):
        result = subprocess.run(
            [
                "python3",
                "tests/task-wdt-idle/run.py",
                "--workspace",
                str(self.WEST_TOPDIR),
                "--output",
                str(self.BUILD_DIR / "task-wdt-idle"),
            ],
            cwd=THIS_DIR,
            capture_output=True,
            text=True,
        )
        (self.BUILD_DIR / "test_task_wdt_idle.log").write_text(
            result.stdout + result.stderr
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "two idle timer variants x3; old-100ms and unsafe-fallback-off controls x3",
            result.stdout,
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        result = run_west(
            [
                "zmk-build",
                "tests/zmk-config",
                "-q",
                "-P",
                "1",
                "-d",
                str(self.BUILD_DIR),
            ]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            if artifact == "module_watchdog_split_central_memory":
                usage = central_artifact_usage(artifact_dir, THIS_DIR)
                self.assertLessEqual(
                    usage["static_ram_bytes"],
                    WATCHDOG_CENTRAL_STATIC_RAM_MAX,
                    "Watchdog central static RAM budget exceeded",
                )
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            self.assertNotIn(
                "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y",
                config_path.read_text().splitlines(),
                f"{artifact}: native input fixture selected on a board",
            )
            self.assertNotIn(
                "CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y",
                config_path.read_text().splitlines(),
                f"{artifact}: native sanitizer fixture selected on a board",
            )
            self.assertNotIn(
                "amgskobo_watchdog_test_kscan",
                device_tree_path.read_text(),
                f"{artifact}: native input devicetree on a board",
            )
            self.assertNotIn(
                "/tests/native-input/kscan.c",
                (artifact_dir.parent / "build.ninja").read_text(),
                f"{artifact}: native input source compiled on a board",
            )
            if "CONFIG_ZMK_WATCHDOG=y" in entries.config:
                effective = config_path.read_text().splitlines()
                self._test_strings_in_file(
                    config_path,
                    ["CONFIG_ZMK_WATCHDOG_DIAGNOSTICS=y"],
                    f"{artifact} freeze trace",
                )
                if "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y" in effective:
                    self._test_strings_in_file(
                        config_path,
                        [
                            "CONFIG_ZMK_WATCHDOG_FREEZE_TRACE=y",
                            "CONFIG_THREAD_STACK_INFO=y",
                            "CONFIG_TASK_WDT_HW_FALLBACK=y",
                            (
                                "CONFIG_TASK_WDT_MIN_TIMEOUT=100"
                                if artifact == "module_watchdog_explicit_fallback"
                                else "CONFIG_TASK_WDT_MIN_TIMEOUT=10000"
                            ),
                        ],
                        f"{artifact} software monitor defaults",
                    )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()
