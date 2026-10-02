#!/usr/bin/env bash
# Run the test suite for one preset. Usage: scripts/test.sh [preset] [ctest args...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/env.sh
source "${ROOT}/scripts/env.sh"

PRESET="${1:-release}"
shift || true
cmake --build --preset "$PRESET" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
ctest --preset "$PRESET" "$@"