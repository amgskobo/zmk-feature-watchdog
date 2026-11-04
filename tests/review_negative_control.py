"""Restore one reviewed Watchdog defect in an isolated owner source copy."""

import argparse
from pathlib import Path


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise SystemExit("expected exactly one reviewed source boundary")
    return source.replace(before, after)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("owner", type=Path)
    parser.add_argument(
        "fault",
        choices=(
            "relay-drain",
            "fifo-overwrite",
            "web-lock",
            "sdk-timeout",
            "delete-zero",
        ),
    )
    args = parser.parse_args()
    root = args.owner.resolve()
    owner = Path(__file__).resolve().parents[1]
    if (
        root.is_relative_to(owner)
        or not (root / "src/split/watchdog_relay_peripheral.c").is_file()
    ):
        raise SystemExit("negative control requires an isolated owner source copy")
    if args.fault in ("relay-drain", "fifo-overwrite"):
        target = root / "src/split/watchdog_relay_peripheral.c"
    else:
        target = root / "web/src/IncidentsSection.tsx"
    source = target.read_text()
    if args.fault == "relay-drain":
        source = replace_once(
            source,
            "    if (k_msgq_get(&watchdog_relay_requests, &request, K_NO_WAIT) != 0) {\n        return;\n    }",
            "    while (k_msgq_get(&watchdog_relay_requests, &request, K_NO_WAIT) == 0) {",
        )
        start = source.index("next_request:")
        end = source.index("\n}\n\nK_WORK_DEFINE(watchdog_relay_process_work", start)
        source = source[:start] + "next_request:\n        ;\n    }" + source[end:]
    elif args.fault == "fifo-overwrite":
        source = replace_once(
            source,
            "    if (k_msgq_put(&watchdog_relay_requests, ev, K_NO_WAIT) != 0) {",
            "    k_msgq_purge(&watchdog_relay_requests); /* Lose an admitted request. */\n    if (k_msgq_put(&watchdog_relay_requests, ev, K_NO_WAIT) != 0) {",
        )
    elif args.fault == "web-lock":
        source = replace_once(source, "!subscribed ||", "!subscribed || true ||")
    elif args.fault == "sdk-timeout":
        source = replace_once(source, "timeout: RPC_TIMEOUT_MS", "timeout: 5000")
    else:
        source = replace_once(
            source, "if (deleted === 0)", "if (false && deleted === 0)"
        )
        source = replace_once(
            source,
            "} else if (deleted !== expected)",
            "} else if (deleted !== 0 && deleted !== expected)",
        )
    target.write_text(source)
    print("Restored only the selected reviewed defect in isolated owner:", args.fault)


if __name__ == "__main__":
    main()
