#include <nanobench.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/catalog.hpp>

namespace orangutan::bench {

namespace {

[[nodiscard]] core::ToolDef make_tool(std::uint32_t index) {
  return core::ToolDef{
      .name = "tool." + std::to_string(index),
      .description = "Synthetic catalog tool " + std::to_string(index) + ".",
      .input_schema_json =
          R"({"type":"object","properties":{"path":{"type":"string"},"max_bytes":{"type":"integer","minimum":1}},"required":["path"],"additionalProperties":false})",
      .required_capabilities = {core::Capability::read_file},
  };
}

[[nodiscard]] std::vector<core::ToolDef> make_catalog() {
  std::vector<core::ToolDef> defs;
  defs.reserve(32);
  for (std::uint32_t i = 0; i < 32; ++i) {
    defs.push_back(make_tool(i));
  }
  return defs;
}

}  // namespace

void register_tool_catalog(ankerl::nanobench::Bench& bench) {
  const auto defs = make_catalog();

  bench.run("catalog.select_all_32_tools", [&defs] {
    const auto selected = tool::select_tools(defs);
    if (!selected) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(*selected);
  });

  const std::vector<std::string> names{"tool.1", "tool.5", "tool.10", "tool.20"};
  bench.run("catalog.select_subset_32_tools", [&defs, &names] {
    const auto selected = tool::select_tools(defs, names);
    if (!selected) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(*selected);
  });
}

}  // namespace orangutan::bench
