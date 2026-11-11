"""Account the central relay's linked static RAM, including kernel objects."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


WATCHDOG_CENTRAL_STATIC_RAM_MAX = 5760


def central_static_usage(text):
    regions = []
    for match in re.finditer(
        r"^\s*(?:RAM|SRAM)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+\S+\s*$",
        text,
        re.M,
    ):
        origin, length = (int(value, 16) for value in match.groups())
        if length:
            regions.append((origin, origin + length))
    if not regions:
        raise AssertionError("central static RAM: missing linked RAM region")
    sections = []
    pending = None
    for line in text.splitlines():
        same = re.match(
            r"^\s+(\.\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)$",
            line,
        )
        wrapped = re.match(r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)$", line)
        if same:
            name, address, size, owner = same.groups()
        elif wrapped and pending:
            name = pending
            address, size, owner = wrapped.groups()
        else:
            declaration = re.fullmatch(r"\s+(\.\S+)\s*", line)
            pending = declaration.group(1) if declaration else None
            continue
        pending = None
        if not owner.endswith("(watchdog_relay_central.c.obj)"):
            continue
        address, size = int(address, 16), int(size, 16)
        if not size or not any(start <= address < end for start, end in regions):
            continue  # Discarded sections and ROM descriptors do not use RAM.
        if not any(
            start <= address and address + size <= end for start, end in regions
        ):
            raise AssertionError("central static RAM: section exceeds RAM region")
        if any(
            address < row["address"] + row["bytes"] and row["address"] < address + size
            for row in sections
        ):
            raise AssertionError("central static RAM: overlapping/duplicate map rows")
        sections.append({"section": name, "address": address, "bytes": size})
    if not sections:
        raise AssertionError("central static RAM: missing central relay input object")
    return {
        "static_ram_bytes": sum(row["bytes"] for row in sections),
        "budget_bytes": WATCHDOG_CENTRAL_STATIC_RAM_MAX,
        "sections": sections,
        "scope": "Linked central relay input sections in RAM, including FIFO/noinit/kernel objects",
    }


def central_artifact_usage(artifact_dir, owner):
    """Require a production central and its actual compiled owner before accounting."""
    artifact_dir, owner = Path(artifact_dir), Path(owner).resolve()
    config = (artifact_dir / ".config").read_text().splitlines()
    for required in [
        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
        "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY=y",
        "# CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST is not set",
    ]:
        if required not in config:
            raise AssertionError(
                "central static RAM: production role missing " + required
            )
    cache = (artifact_dir.parent / "CMakeCache.txt").read_text()
    nm = re.search(r"^CMAKE_NM:FILEPATH=(.+)$", cache, re.M)
    if not nm or not Path(nm.group(1)).is_absolute():
        raise AssertionError("central static RAM: missing selected toolchain nm")
    symbols = subprocess.run(
        [nm.group(1), "--defined-only", str(artifact_dir / "zmk.elf")],
        capture_output=True,
        text=True,
        check=True,
    )
    (artifact_dir.parent / "central-production-symbols.txt").write_text(
        symbols.stdout + symbols.stderr
    )
    if "watchdog_relay_dispatch_request" not in symbols.stdout:
        raise AssertionError("central static RAM: missing linked production dispatcher")
    for forbidden in ["watchdog_split_relay_central_test_init", " resp1.", " resp2."]:
        if forbidden in symbols.stdout:
            raise AssertionError(
                "central static RAM: linked boot-scenario symbol " + forbidden
            )
    commands = json.loads((artifact_dir.parent / "compile_commands.json").read_text())
    source = owner / "src/split/watchdog_relay_central.c"
    central = [row for row in commands if Path(row["file"]).resolve() == source]
    if len(central) != 1:
        raise AssertionError(
            "central static RAM: proof needs the actual compiled owner C"
        )
    usage = central_static_usage((artifact_dir / "zmk.map").read_text())
    usage["compiled_owner"] = central[0]
    usage["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
    usage["elf_sha256"] = hashlib.sha256(
        (artifact_dir / "zmk.elf").read_bytes()
    ).hexdigest()
    (artifact_dir.parent / "watchdog-central-static-ram.json").write_text(
        json.dumps(usage, indent=2) + "\n"
    )
    return usage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifact", type=Path, required=True, help="ARM role's zephyr directory"
    )
    parser.add_argument(
        "--owner", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    usage = central_artifact_usage(args.artifact, args.owner)
    assert usage["static_ram_bytes"] <= WATCHDOG_CENTRAL_STATIC_RAM_MAX, (
        "Watchdog central static RAM budget exceeded"
    )
    print(f"Watchdog central static RAM: PASS {usage['static_ram_bytes']}B")


if __name__ == "__main__":
    main()
