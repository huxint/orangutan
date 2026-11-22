// bench/provider/main.cpp — nanobench entry point for oran-provider.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_cache_requests(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-provider").relative(true);
  bench.unit("mapping");
  bench.minEpochIterations(250'000);
  bench.warmup(20);

  orangutan::bench::register_cache_requests(bench);

  std::println();
  return 0;
}
