#!/usr/bin/env bash
# clang-tidy over all first-party sources.
#   scripts/tidy.sh          report
#   scripts/tidy.sh --fix    apply fixes
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/env.sh
source "${ROOT}/scripts/env.sh"

TIDY="${ROOT}/.venv/bin/clang-tidy"
[[ -x "$TIDY" ]] || { echo "run scripts/bootstrap.sh first" >&2; exit 1; }

BUILD_DIR="${ROOT}/build/tidy"
# Compile commands are the only reliable way to give clang-tidy the exact
# flags each target was built with; a separate config keeps tidy from fighting
# the sanitizers.
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DLOB_BUILD_TESTS=ON -DLOB_BUILD_BENCHES=OFF -DLOB_BUILD_FUZZERS=OFF \
      >/dev/null

cd "$BUILD_DIR"
mapfile -t FILES < <(find "$ROOT/src" "$ROOT/tools" -type f -name '*.cpp' | sort)
if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "no sources found"
  exit 0
fi

# clang-tidy embeds its own clang and therefore does not inherit the host
# compiler's implicit standard-library search path, so '#include <compare>' is
# reported as a missing file. Ask the real compiler where its headers live and
# forward them; this keeps the script portable instead of hardcoding a macOS
# SDK path that would break the Linux CI leg.
CXX_BIN="${CXX:-c++}"
mapfile -t SYSINCLUDES < <("$CXX_BIN" -E -x c++ /dev/null -v 2>&1 \
  | sed -n '/#include <\.\.\.> search starts here:/,/End of search list\./p' \
  | sed -e '1d' -e '$d' | tr -d ' ' | grep '^/' || true)

EXTRA=()
for d in "${SYSINCLUDES[@]:-}"; do
  [[ -n "$d" ]] && EXTRA+=(--extra-arg=-isystem --extra-arg="$d")
done

printf 'clang-tidy on %d translation units (%d system include paths forwarded)\n' \
  "${#FILES[@]}" "$(( ${#EXTRA[@]} / 2 ))"
"$TIDY" -p "$BUILD_DIR" "${EXTRA[@]}" "${FILES[@]}" "$@"