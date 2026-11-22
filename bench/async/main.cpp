// bench/async/main.cpp — registers and runs the oran-async nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_channel_ping_pong(ankerl::nanobench::Bench&);
}

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-async");
  b.unit("batch");
  b.minEpochIterations(30'000);
  b.warmup(100);

  orangutan::bench::register_channel_ping_pong(b);

  std::println();
  return 0;
}
