// bench/agent/main.cpp — nanobench entry point for oran-agent.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_scheduler_overhead(ankerl::nanobench::Bench& bench);
void register_scheduler_audit_fanout(ankerl::nanobench::Bench& bench);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-agent").relative(true);
  bench.unit("fixture");
  bench.minEpochIterations(12'000);
  bench.warmup(20);

  orangutan::bench::register_scheduler_overhead(bench);
  // Registered last: this scenario lowers `minEpochIterations` for its
  // SQLite-backed run, which would otherwise dominate wall time.
  orangutan::bench::register_scheduler_audit_fanout(bench);

  std::println();
  return 0;
}
