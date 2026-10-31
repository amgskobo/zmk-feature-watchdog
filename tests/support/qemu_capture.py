"""Capture completion is a complete terminal record, not a UART prefix."""

import time


def complete_terminal_record(text: str) -> bool:
    return any(
        line.startswith("RESULT:") and line.endswith("\n")
        for line in text.splitlines(keepends=True)
    )


def wait_for_terminal_record(
    process,
    read_text,
    timeout=20,
    poll_interval=0.1,
    clock=time.monotonic,
    pause=time.sleep,
) -> bool:
    deadline = clock() + timeout
    while clock() < deadline:
        if complete_terminal_record(read_text()):
            return True
        if process.poll() is not None:
            return False
        pause(poll_interval)
    return False
