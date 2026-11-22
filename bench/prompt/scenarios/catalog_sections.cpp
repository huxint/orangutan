#include <nanobench.h>

#include <span>
#include <string>
#include <vector>

#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

namespace orangutan::bench {

void register_catalog_sections(ankerl::nanobench::Bench& bench) {
  const std::vector<core::ToolDef> tools{
      core::ToolDef::with_no_input("AgentRun", "Create a child for the supplied task"),
      core::ToolDef::with_no_input("FileEdit", "Edit a file"),
      core::ToolDef::with_no_input("FileRead", "Read a file"),
      core::ToolDef::with_no_input("FileWrite", "Write a file"),
      core::ToolDef::with_no_input("MemoryForget", "Forget a note"),
      core::ToolDef::with_no_input("MemoryRecall", "Read a note"),
      core::ToolDef::with_no_input("MemoryRemember", "Save a note"),
  };

  bench.run("prompt.render_native_catalog", [&tools] {
    const auto rendered = prompt::render({.system_preamble = "system", .tools = tools});
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });

  bench.run("prompt.render_native_subset", [&tools] {
    const auto rendered = prompt::render({.system_preamble = "system", .tools = std::span{tools}.first(2)});
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_hash);
    ankerl::nanobench::doNotOptimizeAway(rendered.prefix_bytes);
  });

  const std::string preamble(4096, 'p');
  const std::string memory(8192, 'm');
  const auto inputs = prompt::RenderInputs{
      .system_preamble = preamble,
      .tools = tools,
      .memory_framing = memory,
  };
  bench.unit("8 iterations").relative(false);
  bench.run("prompt.rebuild_prefix_each_iteration", [&inputs] {
    for (int iteration = 0; iteration < 8; ++iteration) {
      const auto rendered = prompt::render(inputs);
      ankerl::nanobench::doNotOptimizeAway(rendered);
    }
  });
  bench.run("prompt.reuse_prefix_per_turn", [&inputs] {
    const auto rendered = prompt::render(inputs);
    for (int iteration = 0; iteration < 8; ++iteration) {
      ankerl::nanobench::doNotOptimizeAway(rendered);
    }
  });
}

}  // namespace orangutan::bench
