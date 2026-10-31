#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
mode="${ZMK_TEST_MODE:-quick}"
case "$mode" in
    quick|full) ;;
    *) echo "ZMK_TEST_MODE must be quick or full" >&2; exit 2 ;;
esac
root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
variants=(sanitized)
[ "$mode" != full ] || variants=(optimized sanitized)
# Policy variants: the default (a full store drops), the overwrite-oldest option with its default wear guard
# (an incident earlier than 10 s after boot is still dropped), without the guard (0) and with a guard of 1 s.
policies=("" "-DCONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST=1 -DCONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S=10"
          "-DCONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST=1 -DCONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S=0"
          "-DCONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST=1 -DCONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S=1")
run_store() { # capacity, extra flags
    for variant in "${variants[@]}"; do
        flags=(-O2)
        if [ "$variant" = sanitized ]; then
            flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
        fi
        # shellcheck disable=SC2086
        cc -std=c11 -Wall -Wextra -Werror "${flags[@]}" -DCONFIG_ZMK_WATCHDOG_MAX_INCIDENTS="$1" $2 \
            -I"$root/tests/store" -I"$root/include" "$root/tests/store/store_test.c" -o "$work/test"
        ASAN_OPTIONS=detect_leaks=0 "$work/test"
    done
}
for capacity in 16 1; do
    for policy in "${policies[@]}"; do
        run_store "$capacity" "$policy"
    done
done
# Capacities 2 and 3: only the overwrite scenario (the others were written for 1 and 16).
for capacity in 2 3; do
    for policy in "${policies[@]:1}"; do
        run_store "$capacity" "$policy -DWATCHDOG_STORE_OVERWRITE_ONLY=1"
    done
done

python3 -m unittest discover -s "$root/tests" -p test_timeout_width.py -v
