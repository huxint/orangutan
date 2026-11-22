// bench/tool/main.cpp — registers and runs the oran-tool nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_tool_dispatch(ankerl::nanobench::Bench&);
void register_tool_file_write(ankerl::nanobench::Bench&);
void register_tool_file_edit(ankerl::nanobench::Bench&);
void register_tool_approval(ankerl::nanobench::Bench&);
void register_tool_hooks(ankerl::nanobench::Bench&);
void register_tool_catalog(ankerl::nanobench::Bench&);
void register_tool_output(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-tool");
  b.unit("dispatch");
  b.minEpochIterations(2000);
  b.warmup(10);

  orangutan::bench::register_tool_dispatch(b);
  orangutan::bench::register_tool_file_write(b);
  orangutan::bench::register_tool_file_edit(b);
  orangutan::bench::register_tool_approval(b);
  orangutan::bench::register_tool_hooks(b);
  orangutan::bench::register_tool_catalog(b);
  orangutan::bench::register_tool_output(b);

  std::println();
  return 0;
}
