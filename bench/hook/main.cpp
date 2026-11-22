// bench/hook/main.cpp — registers and runs the oran-hook nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_hook_bus(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-hook");
  b.unit("publish");
  b.minEpochIterations(20000);
  b.warmup(50);

  orangutan::bench::register_hook_bus(b);

  std::println();
  return 0;
}
