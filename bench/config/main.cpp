// bench/config/main.cpp — registers and runs the oran-config nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_loading(ankerl::nanobench::Bench&);
void register_permissions(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  {
    ankerl::nanobench::Bench b;
    b.title("bench-config");
    b.unit("load");
    b.minEpochIterations(12000);
    b.warmup(100);

    orangutan::bench::register_loading(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-config/permissions");
    b.unit("parse");
    b.minEpochIterations(8000);
    b.warmup(200);

    orangutan::bench::register_permissions(b);
  }

  std::println();
  return 0;
}
