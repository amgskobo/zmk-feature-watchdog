#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

if [[ -n ${WATCHDOG_NATIVE_ARTIFACTS:-} && ( -e "$WATCHDOG_NATIVE_ARTIFACTS" || -L "$WATCHDOG_NATIVE_ARTIFACTS" ) ]]; then
    echo "Use a fresh WATCHDOG_NATIVE_ARTIFACTS directory; existing evidence is preserved" >&2
    exit 2
fi
root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$(west list zephyr -f '{abspath}')}"
zmk_root="${ZMK_ROOT:-$(west list zmk -f '{abspath}')}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
if ! west build -p=always -b native_sim/native/64 -s "$root/tests/concurrency" \
     -d "$work/build" -- -DWEST=OFF -DZEPHYR_MODULES= -DZMK_ROOT="$zmk_root" > "$work/build.log" 2>&1; then
    cat "$work/build.log" >&2
    exit 1
fi
if [ -n "${WATCHDOG_NATIVE_ARTIFACTS:-}" ]; then
    mkdir -p "$WATCHDOG_NATIVE_ARTIFACTS"
    cp "$work/build.log" "$work/build/CMakeCache.txt" "$WATCHDOG_NATIVE_ARTIFACTS/"
    cp "$work/build/zephyr/.config" "$WATCHDOG_NATIVE_ARTIFACTS/config"
    cp "$work/build/zephyr/zephyr.exe" "$work/build/zephyr/zephyr.elf" "$WATCHDOG_NATIVE_ARTIFACTS/"
fi
for run in 1 2 3; do
    if [ -n "${WATCHDOG_NATIVE_ARTIFACTS:-}" ]; then
        timeout 15 "$work/build/zephyr/zephyr.exe" | tee "$WATCHDOG_NATIVE_ARTIFACTS/run-$run.log"
    else
        timeout 15 "$work/build/zephyr/zephyr.exe"
    fi
done
