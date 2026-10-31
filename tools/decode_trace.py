#!/usr/bin/env python3
"""Decode a copied watchdog report using an explicitly selected firmware ELF."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

EVENTS = {
    1: "boot",
    2: "active profile",
    3: "endpoint",
    4: "USB state",
    10: "BT_CLR begin",
    11: "BT_CLR returned",
    12: "unpair begin",
    13: "unpair returned",
    14: "disconnect requested (handle)",
    15: "disconnect returned",
    16: "connected (handle/error)",
    17: "disconnected (reason)",
    18: "security (error/level)",
    19: "disconnect request reason",
    20: "monitor arm result (queue index)",
    21: "pending conversion",
    22: "reset audit error",
}


def byte_array(raw, name):
    if not isinstance(raw, list) or any(
        type(x) is not int or not 0 <= x <= 255 for x in raw
    ):
        raise ValueError(f"{name} must be a byte array")
    return bytes(raw)


def decode_diagnostics(raw):
    data = byte_array(raw, "diagnostics")
    if not data:
        return None
    if len(data) != 80 or data[0] != 1 or data[1] > 7 or data[2] & ~7 or data[3] & ~1:
        raise ValueError("unknown diagnostic format")
    events = []
    for index in range(data[1]):
        time_ms, event, profile, value = struct.unpack_from(
            "<IBBH", data, 24 + index * 8
        )
        result = (
            value - 65536 if event in (13, 15, 20, 21, 22) and value >= 32768 else value
        )
        events.append(
            dict(
                time_ms=time_ms,
                event=event,
                name=EVENTS.get(event, "unknown"),
                profile=profile,
                value=value,
                result=result,
            )
        )
    return dict(
        build_tag=f"{struct.unpack_from('<I', data, 4)[0]:08x}",
        flags=data[2],
        profile=data[8],
        endpoint=data[9],
        usb=data[10],
        observed_ble=bool(data[3]),
        links=data[11],
        dropped=struct.unpack_from("<I", data, 12)[0],
        cfsr=struct.unpack_from("<I", data, 16)[0],
        hfsr=struct.unpack_from("<I", data, 20)[0],
        events=events,
    )


def decode_fatal_context(raw):
    data = byte_array(raw, "fatal context")
    if not data:
        return None
    if len(data) != 24:
        raise ValueError("unknown fatal context format")
    registers = dict(
        zip(["r0", "r1", "r2", "r3", "r12", "xpsr"], struct.unpack("<6I", data))
    )
    registers["exception_number"] = registers["xpsr"] & 511
    return registers


def decode_capture_context(raw):
    data = byte_array(raw, "capture context")
    if not data:
        return None
    if len(data) != 192 or data[0] != 1 or data[1] & ~127 or data[2] > 6 or data[3] > 4:
        raise ValueError("unknown common capture format")

    def word(at):
        return struct.unpack_from("<I", data, at)[0]

    if word(92) & ~1:
        raise ValueError("unknown common capture capabilities")
    operations = []
    for index in range(data[3]):
        at = 96 + index * 24
        seq, ms, site, domain, event, arg0, arg1 = struct.unpack_from(
            "<IIIHHII", data, at
        )
        operations.append(
            dict(
                sequence=seq,
                time_ms=ms,
                site=site,
                domain=domain,
                event=event,
                arg0=arg0,
                arg1=arg1,
            )
        )
    return dict(
        flags=data[1],
        time_ms=word(4) if word(92) & 1 else None,
        assertion_site=word(8),
        assertion_line=word(12),
        stack_pointer=word(16),
        mmfar=word(20),
        bfar=word(24),
        icsr=word(28),
        callee={f"r{i + 4}": word(32 + i * 4) for i in range(8)}
        if data[1] & 2
        else None,
        addresses=[word(64 + i * 4) for i in range(data[2])],
        dropped=word(88),
        operations=operations,
    )


def decode_runtime(status):
    if not isinstance(status, dict):
        return None
    flags = status.get("runtimeFlags", 0)
    if type(flags) is not int or not 0 <= flags <= 0xFFFFFFFF:
        return dict(raw_status=status, error="invalid runtime runtimeFlags")
    if not flags & 1:
        return None
    result = dict(
        raw_status=status,
        note="Previous checkpoint is boot evidence, not a crash or CPU address.",
    )
    try:
        for key in ["buildTag", "runtimeFlags", "resetCause"]:
            if type(status.get(key)) is not int or not 0 <= status[key] <= 0xFFFFFFFF:
                raise ValueError(f"invalid runtime {key}")
        result["running_build_tag"] = f"{status['buildTag']:08x}"
        result["previous_diagnostics"] = decode_diagnostics(
            status.get("previousDiagnostics", [])
        )
    except ValueError as exc:
        result["error"] = str(exc)
    return result


def decode_trace(raw):
    if not isinstance(raw, list) or any(
        type(x) is not int or not 0 <= x <= 255 for x in raw
    ):
        raise ValueError("trace must be a byte array")
    if not raw:
        return None
    if len(raw) != 36 or raw[0] != 1 or raw[3] > 6 or raw[2] & ~31:
        raise ValueError("unknown trace format")
    if raw[3] and not raw[2] & 1:
        raise ValueError("addresses without a valid frame")
    words = struct.unpack("<8I", bytes(raw[4:]))
    return dict(
        state=raw[1],
        flags=raw[2],
        wait_queue=words[0],
        stack_pointer=words[1],
        addresses=list(words[2 : 2 + raw[3]]),
    )


def record_error(item, field, message):
    item[field] = str(message)
    item.setdefault("error", str(message))


def symbol_requests(item):
    """Collect raw code candidates after the incident identity gate succeeds."""
    fatal = item.get("fatal")
    trace = item.get("trace")
    addresses = trace["addresses"] if trace else []
    if fatal:
        addresses = [fatal.get("pc", 0), fatal.get("lr", 0)]
    requests = [
        (
            raw,
            ("fatal PC" if i == 0 else "fatal LR")
            if fatal
            else (
                ("interrupted PC" if trace["flags"] & 2 else "saved PC")
                if i == 0
                else "return candidate"
            ),
            i != 0,
        )
        for i, raw in enumerate(addresses)
    ]
    context = item.get("capture_context")
    if context:
        # Capture filters out non-code PCs, so slot zero can already be LR.
        # Classify candidates using the original frame rather than position.
        saved_pc = (
            fatal.get("pc")
            if fatal
            else (addresses[0] if trace and addresses and trace["flags"] & 1 else None)
        )
        if context["flags"] & 16:
            requests.append((context["assertion_site"], "assertion call site", True))
        requests.extend(
            (raw, "common capture address candidate", raw != saved_pc)
            for raw in context["addresses"]
        )
        requests.extend(
            (op["site"], "metadata operation call site", True)
            for op in context["operations"]
            if op["site"]
        )
    return requests


def symbolize_requests(requests, elf, addr2line):
    """Normalize code/return addresses and resolve against the selected ELF."""
    symbols = []
    for raw, kind, is_return in requests:
        # PC identifies an instruction. A Thumb return address identifies
        # the next instruction; resolve inside its preceding call instead.
        if type(raw) is not int or not 0 < raw <= 0xFFFFFFFF:
            symbols.append(dict(raw=raw, error="missing or invalid code address"))
            continue
        address = raw & ~1
        if is_return:
            address -= 2
        if address < 0:
            symbols.append(dict(raw=raw, error="invalid code address"))
            continue
        proc = subprocess.run(
            [
                addr2line,
                "-e",
                str(elf),
                "-f",
                "-C",
                "-i",
                hex(address),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        symbols.append(
            dict(
                raw=raw,
                lookup=address,
                kind=kind,
                symbol=proc.stdout.splitlines(),
            )
        )
    return symbols


def decode_incident(incident, manifest=None):
    """Decode independent metadata without running an external symbol tool."""
    if not isinstance(incident, dict):
        return dict(raw_incident=incident, error="incident must be an object"), []
    freeze = incident.get("freeze")
    fatal = incident.get("fatal")
    # A malformed detail must not raise before the identity check or abort
    # later incidents. Keep it in raw_incident and diagnose it below.
    freeze = freeze if isinstance(freeze, dict) else None
    fatal = fatal if isinstance(fatal, dict) else None
    item = dict(
        raw_incident=incident,
        id=incident.get("id"),
        source=incident.get("source"),
        bootOrdinal=incident.get("bootOrdinal"),
        uptimeS=incident.get("uptimeS"),
        captureFlags=incident.get("captureFlags", 0),
        type=incident.get("type"),
        queue=freeze.get("queueName") if freeze else None,
        raw_trace=freeze.get("trace", []) if freeze else [],
        raw_diagnostics=incident.get("diagnostics", []),
        raw_capture_context=incident.get("captureContext", []),
    )
    try:
        item["diagnostics"] = decode_diagnostics(item["raw_diagnostics"])
        if (
            manifest
            and item["diagnostics"]
            and item["diagnostics"]["build_tag"] != manifest["build_tag"]
        ):
            raise ValueError(
                "Incident build tag differs from selected ELF manifest; no addresses decoded"
            )
    except ValueError as exc:
        item["error"] = str(exc)
        return item, []
    for field in ["freeze", "fatal"]:
        detail = incident.get(field)
        if detail is not None and not isinstance(detail, dict):
            record_error(item, field + "_error", f"{field} must be an object")
    trace = None
    try:
        trace = decode_trace(item["raw_trace"])
    except ValueError as exc:
        record_error(item, "trace_error", exc)
    item["trace"] = trace
    if fatal:
        item["fatal"] = fatal
        item["fatal_context"] = None
        try:
            item["fatal_context"] = decode_fatal_context(fatal.get("context", []))
        except ValueError as exc:
            record_error(item, "fatal_context_error", exc)
    # The optional extension may be newer or damaged while the primary
    # fatal/trace remains usable. Preserve its raw bytes and report its
    # error separately, after the existing build-identity gate succeeds.
    item["capture_context"] = None
    try:
        flags = item["captureFlags"]
        if type(flags) is not int or not 0 <= flags <= 0xFFFFFFFF:
            raise ValueError("captureFlags must be a uint32")
        context = decode_capture_context(item["raw_capture_context"])
        if flags & 4 and not context:
            raise ValueError("missing common capture extension")
        item["capture_context"] = context
    except ValueError as exc:
        record_error(item, "capture_context_error", exc)
    return item, symbol_requests(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument(
        "--expected-elf-sha256",
        help="Reject an ELF that differs from your build manifest",
    )
    parser.add_argument("--addr2line", default="arm-zephyr-eabi-addr2line")
    parser.add_argument(
        "--build-manifest",
        type=Path,
        help="Post-link manifest with build_tag and elf_sha256; reject other builds before symbol lookup",
    )
    args = parser.parse_args()
    digest = hashlib.sha256(args.elf.read_bytes()).hexdigest()
    if args.expected_elf_sha256 and digest != args.expected_elf_sha256.lower():
        parser.error("ELF SHA-256 mismatch; no addresses decoded")
    manifest = None
    if args.build_manifest:
        manifest = json.loads(args.build_manifest.read_text(encoding="utf-8-sig"))
        if (
            not isinstance(manifest, dict)
            or manifest.get("elf_sha256") != digest
            or not isinstance(manifest.get("build_tag"), str)
        ):
            parser.error("Build manifest/ELF mismatch; no addresses decoded")
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    if not isinstance(report, dict) or report.get("format") != "zmk-watchdog-report-v1":
        parser.error("expected a copied zmk-watchdog-report-v1 report")
    incidents = report.get("incidents", [])
    if not isinstance(incidents, list):
        parser.error("report incidents must be an array")
    decoded = []
    for incident in incidents:
        item, requests = decode_incident(incident, manifest)
        if requests:
            item["symbols"] = symbolize_requests(requests, args.elf, args.addr2line)
        decoded.append(item)
    print(
        json.dumps(
            dict(
                elf=str(args.elf),
                elf_sha256=digest,
                elf_identity="hash checked"
                if args.expected_elf_sha256 or manifest
                else "owner must confirm matching firmware",
                note="Return candidates are not a complete unwound stack; wait_queue is a pointer, not a proven owner.",
                incidents=decoded,
                copiedAt=report.get("copiedAt"),
                retrieval=report.get("retrieval"),
                runtime=decode_runtime(report.get("status")),
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
