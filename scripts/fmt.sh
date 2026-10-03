#!/usr/bin/env bash
# clang-format over all first-party sources.
#   scripts/fmt.sh          format in place
#   scripts/fmt.sh --check  verify only (what CI runs)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/env.sh
source "${ROOT}/scripts/env.sh"

FORMATTER="${ROOT}/.venv/bin/clang-format"
[[ -x "$FORMATTER" ]] || { echo "run scripts/bootstrap.sh first" >&2; exit 1; }

# --dry-run alone exits 0 even when it reports violations, which would let CI
# pass on unformatted code; --Werror is what makes the exit code meaningful.
ACTION=(-i)
[[ "${1:-}" == "--check" ]] && ACTION=(--dry-run --Werror)

cd "$ROOT"
# Not mapfile: that is a bash 4 builtin and macOS still ships bash 3.2, so CI
# died with "mapfile: command not found" (exit 127) before formatting anything.
# A read loop works on both.
FILES=()
while IFS= read -r line; do
  FILES+=("$line")
done < <(find include src tools tests bench fuzz \
         -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) \
         2>/dev/null | sort)
if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "no sources found"
  exit 0
fi
printf 'clang-format %s on %d files\n' "${ACTION[*]}" "${#FILES[@]}"
"$FORMATTER" "${ACTION[@]}" "${FILES[@]}"