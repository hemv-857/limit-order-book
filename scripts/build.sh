#!/usr/bin/env bash
# Configure + build one preset. Usage: scripts/build.sh [preset] [targets...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/env.sh
source "${ROOT}/scripts/env.sh"

PRESET="${1:-release}"
shift || true

if [[ ! -x "${ROOT}/.venv/bin/ninja" ]]; then
  echo "run scripts/bootstrap.sh first" >&2
  exit 1
fi

cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" "$@"