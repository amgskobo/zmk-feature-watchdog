#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
exec python3 "$root/tests/store/mutate_overwrite.py" "$@"
