"""Independent fixed-vector admission for the owner's synthetic input boundary."""

import json
import re

PRE_MAIN_TERMINALS = {
    "watchdog": ("watchdog_test_init", "PASS: watchdog_freeze_detect"),
    "studio": ("watchdog_rpc_test_init", "PASS: watchdog_rpc_inject_freeze_recorded"),
    "studio_unsecured": (
        "watchdog_rpc_test_init",
        "PASS: watchdog_rpc_inject_freeze_recorded",
    ),
}


def require_native_runtime_libraries(dynamic):
    for name in ["libasan.so", "libubsan.so"]:
        if not re.search(
            r"Shared library: \[" + re.escape(name) + r"(?:\.\d+)*\]", dynamic
        ):
            raise AssertionError(f"native runtime dependency missing: {name}")


def load_input_oracles(tests, fixtures):
    path = tests / "native-input" / "oracles.json"
    if not path.is_file():
        raise AssertionError("missing native input oracle")
    oracles = json.loads(path.read_text())
    if set(oracles) != set(fixtures):
        raise AssertionError("native input oracle fixture selection mismatch")
    for case, oracle in oracles.items():
        expected = "pre_main_freeze" if case in PRE_MAIN_TERMINALS else "input_complete"
        if oracle.get("boundary") != expected:
            raise AssertionError("native input case boundary mismatch")
        if expected == "pre_main_freeze":
            hook, marker = PRE_MAIN_TERMINALS[case]
            if (
                oracle.get("terminal_hook") != hook
                or oracle.get("terminal_pass") != marker
            ):
                raise AssertionError(
                    "native input pre-main terminal declaration mismatch"
                )
            if oracle["events"] or oracle["hid"] or not oracle.get("configured_events"):
                raise AssertionError(
                    "native input pre-main oracle contradicts its boundary"
                )
    return oracles


def verify_input_trace(raw, oracle):
    if oracle["boundary"] == "pre_main_freeze":
        if any(
            token in raw
            for token in [
                "WATCHDOG_INPUT_EVENT",
                "WATCHDOG_INPUT_COMPLETE",
                "OWNER_TEST_RESULT watchdog_native_input_complete=",
                "on_keymap_binding_",
                "Welcome to ZMK!",
            ]
        ):
            raise AssertionError(
                "native input pre-main outcome contains input/completion/HID/main"
            )
        hook = oracle["terminal_hook"]
        results = list(
            re.finditer(r"\bOWNER_TEST_RESULT " + re.escape(hook) + r"=(-?\d+)", raw)
        )
        marker = oracle["terminal_pass"]
        if len(results) != 1 or results[0].group(1) != "0" or raw.count(marker) != 1:
            raise AssertionError(
                "native input pre-main terminal evidence missing/invalid"
            )
        if raw.index(marker) > results[0].start() or not raw.rstrip().splitlines()[
            -1
        ].endswith("OWNER_TEST_RESULT " + hook + "=0"):
            raise AssertionError("native input pre-main terminal ordering mismatch")
        return {
            "input_status": "NOT APPLICABLE",
            "boundary": "pre_main_freeze",
            "terminal_hook": hook,
            "callbacks": 0,
            "main_observed": False,
        }
    event_pattern = (
        r"^WATCHDOG_INPUT_EVENT index=(\d+) word=([0-9a-fA-F]{8}) "
        r"row=(\d+) column=(\d+) pressed=(\d+) delay_ms=(\d+)$"
    )
    matches = list(re.finditer(event_pattern, raw, re.M))
    events = [
        [int(index), int(word, 16), int(row), int(column), int(pressed), int(delay)]
        for index, word, row, column, pressed, delay in (
            match.groups() for match in matches
        )
    ]
    if events != oracle["events"]:
        raise AssertionError("native input ordered callback trace mismatch")
    completions = re.findall(r"^WATCHDOG_INPUT_COMPLETE count=(\d+)$", raw, re.M)
    if completions != [str(len(oracle["events"]))]:
        raise AssertionError("native input completion mismatch")
    complete = re.search(r"^WATCHDOG_INPUT_COMPLETE count=\d+$", raw, re.M)
    if matches and complete.start() < matches[-1].end():
        raise AssertionError("native input completion precedes callbacks")
    hid = [
        [kind, int(position), code]
        for kind, position, code in re.findall(
            r"zmk: on_keymap_binding_(pressed|released): position (\d+) keycode (0x[0-9a-f]+)",
            raw,
        )
    ]
    if hid != oracle["hid"]:
        raise AssertionError("native input keymap/HID trace mismatch")
    if re.findall(
        r"\bOWNER_TEST_RESULT watchdog_native_input_complete=(-?\d+)", raw
    ) != ["0"]:
        raise AssertionError("native input completion result missing/invalid")
    return {
        "input_status": "PASS",
        "boundary": "input_complete",
        "callbacks": len(events),
        "main_observed": "Welcome to ZMK!" in raw,
    }


def verify_input_build(binary, oracle=None):
    config = (binary.parent / ".config").read_text().splitlines()
    for flag in [
        "CONFIG_ZMK_WATCHDOG_NATIVE_INPUT_FIXTURE=y",
        "CONFIG_ZMK_WATCHDOG_NATIVE_SANITIZER_FIXTURE=y",
        "CONFIG_ASAN=y",
    ]:
        if flag not in config:
            raise AssertionError(f"native input required config missing: {flag}")
    if "CONFIG_ASAN_RECOVER=y" in config or "CONFIG_ZMK_KSCAN_MOCK_DRIVER=y" in config:
        raise AssertionError("native input used recovery or upstream mock driver")
    commands = json.loads((binary.parent.parent / "compile_commands.json").read_text())
    fixture = [
        row for row in commands if row["file"].endswith("/tests/native-input/kscan.c")
    ]
    if len(fixture) != 1 or any(
        row["file"].endswith("/kscan_mock.c") for row in commands
    ):
        raise AssertionError("native input compiled source selection mismatch")
    import shlex

    owner_root = fixture[0]["file"].removesuffix("/tests/native-input/kscan.c")
    for entry in commands:
        # Zephyr also builds its native runner and architecture support with
        # their own toolchain rules. Require instrumentation on this owner and
        # the real core application path exercised by these scenarios.
        if not entry["file"].endswith(".c") or not (
            entry["file"].startswith(owner_root + "/") or "/app/src/" in entry["file"]
        ):
            continue
        argv = entry.get("arguments") or shlex.split(entry["command"])
        sanitizers = set()
        # GCC's undefined-behavior instrumentation recovers by default; a
        # strict application profile must explicitly turn that off.
        recover = {"undefined"}
        for argument in argv:
            if argument.startswith("-fsanitize="):
                sanitizers.update(argument.partition("=")[2].split(","))
            elif argument.startswith("-fno-sanitize="):
                disabled = set(argument.partition("=")[2].split(","))
                if "all" in disabled:
                    sanitizers.clear()
                else:
                    sanitizers.difference_update(disabled)
            elif argument.startswith("-fsanitize-recover="):
                modes = set(argument.partition("=")[2].split(","))
                recover.update({"address", "undefined"} if "all" in modes else modes)
            elif argument.startswith("-fno-sanitize-recover="):
                modes = set(argument.partition("=")[2].split(","))
                recover.difference_update(
                    {"address", "undefined"} if "all" in modes else modes
                )
        if not {"address", "undefined"} <= sanitizers or recover:
            raise AssertionError(
                f"native input strict compiler profile missing: {entry['file']}"
            )
    dts = (binary.parent / "zephyr.dts").read_text()
    if 'compatible = "amgskobo,watchdog-test-kscan"' not in dts:
        raise AssertionError("native input devicetree compatible missing")
    if oracle is not None:
        arrays = re.findall(r"\bevents\s*=\s*<([^>]*)>", dts)
        words = (
            [int(value, 0) for value in arrays[0].split()] if len(arrays) == 1 else []
        )
        expected = oracle.get("configured_events", oracle["events"])
        if words != [event[1] for event in expected]:
            raise AssertionError("native input configured DT word vector mismatch")
        if oracle["boundary"] == "pre_main_freeze":
            for flag in [
                "CONFIG_APPLICATION_INIT_PRIORITY=90",
                "CONFIG_ZMK_WATCHDOG_FREEZE_DETECT=y",
                "CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y",
            ]:
                if flag not in config:
                    raise AssertionError(
                        "native input pre-main configuration mismatch: " + flag
                    )
    nsi = (binary.parent / "NSI/nsi_config").read_text()
    link = next(
        (line for line in nsi.splitlines() if line.startswith("NSI_LINK_OPTIONS:=")), ""
    )
    if (
        "-fsanitize=address,undefined" not in link
        or "-fno-sanitize-recover=all" not in link
    ):
        raise AssertionError("native input strict final-runner link profile missing")
