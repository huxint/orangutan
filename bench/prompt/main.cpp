// bench/prompt/main.cpp — nanobench entry point for oran-prompt.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_catalog_sections(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-prompt").relative(true);
  bench.unit("render");
  bench.minEpochIterations(2'000);
  bench.warmup(20);

  orangutan::bench::register_catalog_sections(bench);

  std::println();
  return 0;
}
