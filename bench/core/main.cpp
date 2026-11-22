// bench/core/main.cpp — registers and runs the oran-core nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_error_construct(ankerl::nanobench::Bench&);
void register_time_scenarios(ankerl::nanobench::Bench&);
void register_message_scenarios(ankerl::nanobench::Bench&);
void register_tool_def_scenarios(ankerl::nanobench::Bench&);
void register_str_utf8_scenarios(ankerl::nanobench::Bench&);
void register_capability_scenarios(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/error");
    b.unit("Error");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_error_construct(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/time");
    b.unit("op");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_time_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/message");
    b.unit("op");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_message_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/tool_def");
    b.unit("ToolDef");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_tool_def_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/str_utf8");
    b.unit("walk");
    b.minEpochIterations(50'000);
    b.warmup(1'000);

    orangutan::bench::register_str_utf8_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-core/capability");
    b.unit("lookup-batch");
    b.minEpochIterations(50'000);
    b.warmup(1'000);

    orangutan::bench::register_capability_scenarios(b);
  }

  std::println();
  return 0;
}
