#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$root/web"
test -d node_modules || { echo "Install the owner web/package-lock.json dependencies first (npm ci)" >&2; exit 2; }
npm test -- --runInBand "$@"
