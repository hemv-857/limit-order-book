#!/usr/bin/env bash
# Run the fuzz targets.
#
# Two drivers, one set of targets. Where libFuzzer's runtime exists the
# coverage-guided drivers are strictly better and are used. Where it does not --
# including this project's default macOS toolchain, see docs/PLAN.md deviation D-2
# -- the portable corpus+mutation driver runs instead, under whatever sanitizer
# preset is selected.
#
# The portable driver is not a consolation prize: it is what runs in CI on the
# default toolchain, and under ASan and UBSan it is a real memory-safety and
# invariant-safety net.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh

preset="${1:-asan-ubsan}"

if [[ -x "build/${preset}/bin/fuzz_codec" ]]; then
  echo "fuzz: libFuzzer available, running coverage-guided targets"
  corpus_dir="fuzz/corpus"
  mkdir -p "$corpus_dir"
  for target in codec journal engine; do
    echo "fuzz: ${target}"
    "./build/${preset}/bin/fuzz_${target}" "${corpus_dir}/${target}" \
      -max_total_time="${FUZZ_SECONDS:-60}" -max_len=4096
  done
  exit 0
fi

echo "fuzz: libFuzzer runtime not available on this toolchain; running the portable driver"
echo "fuzz: this is the driver that always runs -- see docs/PLAN.md deviation D-2"
cmake --preset "$preset" >/dev/null
cmake --build --preset "$preset" --target test_fuzz >/dev/null
"./build/${preset}/bin/test_fuzz" "$@"
