// bench/core/scenarios/tool_def.cpp
//
// A-vs-B coverage for `core::ToolDef` construction:
//
//   1. `core.tool_def_aggregate_init` : aggregate-init with the schema as a
//                                       literal `std::string` (the path the
//                                       agent loop and provider adapters
//                                       will use for real tools).
//   2. `core.tool_def_with_no_input`  : `ToolDef::with_no_input(name, desc)`
//                                       (the fixture path).
//
// This documents the cost of preferring the helper over the bare aggregate
// for parameter-less tools so future callers can pick with eyes open.

#include <nanobench.h>

#include <string>
#include <string_view>

#include <oran/core/tool_def.hpp>

namespace orangutan::bench {

namespace {

using core::ToolDef;

constexpr std::string_view kEmptySchema = R"({"type":"object","properties":{},"additionalProperties":false})";

[[gnu::noinline]] ToolDef make_via_aggregate() {
  return ToolDef{
      .name = "ClockNow",
      .description = "Return current UTC time.",
      .input_schema_json = std::string{kEmptySchema},
      .required_capabilities = {},
  };
}

[[gnu::noinline]] ToolDef make_via_helper() {
  return ToolDef::with_no_input("ClockNow", "Return current UTC time.");
}

}  // namespace

void register_tool_def_scenarios(ankerl::nanobench::Bench& bench) {
  bench.run("core.tool_def_aggregate_init", [&] {
    auto td = make_via_aggregate();
    ankerl::nanobench::doNotOptimizeAway(td);
  });
  bench.run("core.tool_def_with_no_input", [&] {
    auto td = make_via_helper();
    ankerl::nanobench::doNotOptimizeAway(td);
  });
}

}  // namespace orangutan::bench
