add_executable(bench_smoke smoke_benchmark.cpp)
target_link_libraries(bench_smoke PRIVATE lob_core benchmark::benchmark_main lob_warnings)

# The full suite spans every layer, so it links the lot. Benchmarks are release-only
# by intent: a debug build measures the wrong thing.
add_executable(bench_lob bench_lob.cpp)
target_link_libraries(bench_lob PRIVATE lob_gateway lob_runtime lob_protocol lob_journal
                      benchmark::benchmark_main lob_warnings)
