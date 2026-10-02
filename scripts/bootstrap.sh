#!/usr/bin/env bash
# Repo-scoped toolchain bootstrap.
#
# Installs ninja, clang-format and clang-tidy into ./.venv only. Nothing outside
# this repository is modified: no Homebrew, no system pip, no sudo. The reason
# is that the host may not have them (macOS CommandLineTools ships neither) and
# the fix should not require touching a machine's global state.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${ROOT}/.venv"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }

PY="${PYTHON:-python3}"
command -v "$PY" >/dev/null 2>&1 || die "python3 not found (set \$PYTHON to override)"

if [[ ! -d "$VENV" ]]; then
  log "creating ${VENV}"
  "$PY" -m venv "$VENV"
fi

log "installing build tools into ${VENV} (repo-local)"
"${VENV}/bin/python" -m pip install --quiet --upgrade pip
"${VENV}/bin/python" -m pip install --quiet ninja clang-format clang-tidy

log "installed versions"
printf '  ninja        %s\n' "$("${VENV}/bin/ninja" --version 2>/dev/null | head -1)"
printf '  clang-format %s\n' "$("${VENV}/bin/clang-format" --version 2>/dev/null)"
printf '  clang-tidy   %s\n' "$("${VENV}/bin/clang-tidy" --version 2>/dev/null | sed -n '2p' | sed 's/^LLVM version //')"

if ! command -v cmake >/dev/null 2>&1; then
  warn "cmake not found on PATH; install it separately (3.22+ required)"
else
  log "cmake $(cmake --version | head -1 | awk '{print $3}')"
fi

if ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
  warn "no C++ compiler on PATH"
fi

cat <<'EOF'

Done. Source the environment so the repo-local tools are visible:

    source scripts/env.sh

Then build:

    cmake --preset release && cmake --build --preset release -j
EOF