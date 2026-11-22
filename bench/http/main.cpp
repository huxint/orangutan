// bench/http/main.cpp - nanobench entry point for oran-http.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_http_client(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("bench-http").relative(true);
  bench.unit("operation");
  bench.minEpochIterations(1'200'000);
  bench.warmup(20);

  orangutan::bench::register_http_client(bench);

  std::println();
  return 0;
}
