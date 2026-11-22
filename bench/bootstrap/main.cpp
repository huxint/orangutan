#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_permission_config(ankerl::nanobench::Bench&);
void register_runtime_assembly_build(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  {
    ankerl::nanobench::Bench b;
    b.title("bench-bootstrap");
    b.unit("assembly");
    orangutan::bench::register_runtime_assembly_build(b);
  }
  {
    ankerl::nanobench::Bench b;
    b.title("bench-bootstrap/permissions");
    b.unit("RuleSet");
    b.minEpochIterations(30'000);
    b.warmup(1'000);
    orangutan::bench::register_permission_config(b);
  }

  std::println();
  return 0;
}
