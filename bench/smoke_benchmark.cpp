#include <benchmark/benchmark.h>

#include "core/types.hpp"

// Validates that the benchmark harness links against the core library and that
// Google Benchmark reports sane numbers before any real measurement is trusted.
static void BM_TypesBaseline(benchmark::State& state) {
  std::int64_t acc = 0;
  for (auto _ : state) {
    acc += lob::Price{state.range(0)}.value;
    benchmark::DoNotOptimize(acc);
  }
}
BENCHMARK(BM_TypesBaseline)->Arg(1);

BENCHMARK_MAIN();