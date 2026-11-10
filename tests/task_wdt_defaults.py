#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the owner's Task WDT defaults in the unchanged real board Kconfig graph."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def require_defaults(config, fallback, minimum):
    lines = config.splitlines()
    assert (
        "CONFIG_TASK_WDT_HW_FALLBACK=y"
        if fallback
        else "# CONFIG_TASK_WDT_HW_FALLBACK is not set"
    ) in lines, "hardware-fallback override mismatch"
    actual = next(
        (
            line.split("=", 1)[1]
            for line in lines
            if line.startswith("CONFIG_TASK_WDT_MIN_TIMEOUT=")
        ),
        None,
    )
    assert actual == (str(minimum) if minimum is not None else None), (
        "software-monitor minimum-timeout default mismatch"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if args.output.is_symlink() or output.exists():
        parser.error("Use a fresh output directory")
    output.mkdir(parents=True)
    workspace = args.workspace.resolve()
    env = dict(
        os.environ,
        ZEPHYR_BASE=str(workspace / "zephyr"),
        Zephyr_DIR=str(workspace / "zephyr/share/zephyr-package/cmake"),
        GIT_NO_LAZY_FETCH="1",
        GIT_TERMINAL_PROMPT="0",
    )
    listed = subprocess.check_output(
        ["west", "list", "-f", "{abspath}"], cwd=workspace, env=env, text=True
    ).splitlines()
    modules = []
    for value in [*listed, str(ROOT)]:
        path = Path(value)
        if (path / "zephyr/module.yml").is_file() or (
            path / "zephyr/CMakeLists.txt"
        ).is_file():
            if str(path) not in modules:
                modules.append(str(path))
    cases = [
        ("monitor-default", ["CONFIG_ZMK_WATCHDOG=y"], True, 10000),
        (
            "explicit-y",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_TASK_WDT_HW_FALLBACK=y"],
            True,
            10000,
        ),
        (
            "explicit-n",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_TASK_WDT_HW_FALLBACK=n"],
            False,
            None,
        ),
        ("owner-disabled", ["CONFIG_ZMK_WATCHDOG=n", "CONFIG_TASK_WDT=y"], True, 100),
        (
            "freeze-disabled",
            [
                "CONFIG_ZMK_WATCHDOG=y",
                "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=n",
                "CONFIG_TASK_WDT=y",
            ],
            True,
            100,
        ),
        (
            "minimum-explicit",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_TASK_WDT_MIN_TIMEOUT=1000"],
            True,
            1000,
        ),
        (
            "timeout-minimum",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS=10"],
            True,
            10,
        ),
        (
            "timeout-below-cap",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS=9999"],
            True,
            9999,
        ),
        (
            "timeout-above-cap",
            ["CONFIG_ZMK_WATCHDOG=y", "CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS=600000"],
            True,
            10000,
        ),
        ("old-default-control", ["CONFIG_ZMK_WATCHDOG=y"], True, 10000),
    ]
    records = []
    for name, settings, fallback, minimum in cases:
        case = output / name
        case.mkdir()
        conf = case / "fixture.conf"
        conf.write_text("\n".join(settings) + "\n")
        extra = []
        if name == "old-default-control":
            # A deliberately earlier default reproduces the old 100-ms wake.
            # The selected core and owner sources remain unchanged/read-only.
            wrapper = case / "Kconfig"
            wrapper.write_text(
                "config TASK_WDT_MIN_TIMEOUT\n"
                "    default 100 if ZMK_WATCHDOG_FREEZE_DETECT\n\n"
                'source "' + str(workspace / "zmk/app/Kconfig") + '"\n'
            )
            extra.append("-DKCONFIG_ROOT=" + str(wrapper))
        command = [
            "west",
            "build",
            "--cmake-only",
            "-p=always",
            "-s",
            str(workspace / "zmk/app"),
            "-d",
            str(case / "build"),
            "-b",
            "xiao_ble/nrf52840/zmk",
            "--",
            "-DSHIELD=tester_xiao",
            "-DZEPHYR_MODULES=" + ";".join(modules),
            "-DEXTRA_CONF_FILE=" + str(conf),
            *extra,
        ]
        row = dict(name=name, command=command, start=time.time())
        with (case / "configure.log").open("x") as log:
            result = subprocess.run(
                command, cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT
            )
        row.update(exit=result.returncode, end=time.time())
        records.append(row)
        (output / "results.json").write_text(json.dumps(records, indent=2))
        if result.returncode:
            raise RuntimeError(
                "Configuration failed; not a default-control rejection: " + name
            )
        config = (case / "build/zephyr/.config").read_text()
        assert "CONFIG_TASK_WDT=y" in config.splitlines(), name
        if name == "old-default-control":
            try:
                require_defaults(config, fallback, minimum)
            except AssertionError as error:
                assert str(error) == "software-monitor minimum-timeout default mismatch"
                row["expected_rejection"] = str(error)
            else:
                raise AssertionError(
                    "Old default control did not fail the specified assertion"
                )
        else:
            require_defaults(config, fallback, minimum)
        row["config_sha256"] = hashlib.sha256(config.encode()).hexdigest()
        (output / "results.json").write_text(json.dumps(records, indent=2))
    print(
        "PASS: nine actual Kconfig cases; old-default control rejected its named assertion"
    )


if __name__ == "__main__":
    main()
