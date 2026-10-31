#!/usr/bin/env python3
"""Configuration-time diagnostic tag. Full ELF SHA-256 is bound after linking."""

import argparse
import hashlib
import json
import secrets
from pathlib import Path
import subprocess


def git_revision(repo):
    # A checkout git cannot read (no git, not a repository, an unsafe owner)
    # records "unknown" instead of stopping the firmware build over a tag.
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--zephyr", type=Path, required=True)
    parser.add_argument("--zmk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inputs = {}
    for directory in ["src", "include", "proto", "tools"]:
        for path in sorted((args.root / directory).rglob("*")):
            if path.is_file() and path.suffix in {
                ".c",
                ".h",
                ".proto",
                ".options",
                ".py",
            }:
                inputs[path.relative_to(args.root).as_posix()] = hashlib.sha256(
                    path.read_bytes()
                ).hexdigest()
    for name in ["Kconfig", "CMakeLists.txt"]:
        inputs[name] = hashlib.sha256((args.root / name).read_bytes()).hexdigest()
    inputs[".config"] = hashlib.sha256(args.config.read_bytes()).hexdigest()
    config = args.config.read_text()
    # These units used to live below src/ and were therefore hashed above.
    # Include only test inputs selected into this binary, not unrelated suites.
    test_inputs = []
    if "CONFIG_ZMK_WATCHDOG_TEST=y\n" in config:
        test_inputs.append("tests/ztest/watchdog_test.c")
    if "CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST=y\n" in config:
        test_inputs.append("tests/ztest/watchdog_rpc_test.c")
    if "CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST=y\n" in config:
        central = "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y\n" in config
        test_inputs.append(
            f"tests/support/watchdog_relay_assertions_{int(central)}.inc"
        )
        if not central:
            test_inputs.append("tests/support/context_relay_assertions.h")
    if test_inputs:
        test_inputs.append("tests/support/init_result.h")
    for name in test_inputs:
        inputs[name] = hashlib.sha256((args.root / name).read_bytes()).hexdigest()
    for name, repo in [("zephyr", args.zephyr), ("zmk", args.zmk)]:
        inputs[name + "_revision"] = git_revision(repo)
    digest = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    # A fresh configuration nonce also distinguishes binaries whose other
    # modules changed without touching this module or Kconfig. The post-link
    # manifest binds the tag to the complete ELF SHA-256; never trust a 32-bit
    # tag alone as collision-free identity or authentication.
    nonce = secrets.token_hex(16)
    tag = int(hashlib.sha256((digest + nonce).encode()).hexdigest()[:8], 16)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "watchdog_build_id.h").write_text(
        f"#define WATCHDOG_BUILD_TAG 0x{tag:08x}U\n"
    )
    (args.output / "watchdog-build-identity.json").write_text(
        json.dumps(
            dict(
                build_tag=f"{tag:08x}",
                configuration_nonce=nonce,
                source_config_sha256=digest,
                inputs=inputs,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
