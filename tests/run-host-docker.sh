#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
export MSYS_NO_PATHCONV=1
root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
docker run --rm --env ZMK_TEST_MODE="${ZMK_TEST_MODE:-quick}" \
    --volume "$root:/src:ro" "${ZMK_TEST_IMAGE:-zmkfirmware/zmk-build-arm:stable}" \
    bash /src/tests/store/run.sh
