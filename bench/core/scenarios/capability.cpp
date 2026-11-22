// bench/core/scenarios/capability.cpp
//
// A-vs-B coverage for the reflection-backed `core::parse_enum<Capability>`:
//
//   1. `core.capability_parse_linear`        : `core::parse_enum<Capability>` —
//                                              a reflection-expanded linear
//                                              scan over the 20-entry
//                                              capability universe.
//   2. `core.capability_parse_unordered_map` : the same lookup driven by
//                                              an `std::unordered_map<
//                                              std::string_view,
//                                              Capability>` keyed on the
//                                              same spellings.
//
// Both scenarios iterate the same deterministic mix of valid and invalid
// inputs so the comparison documents the cost of the linear-scan path the
// library actually ships vs. the cheapest hash-table alternative. The
// goal is *not* to crown a winner — at 20 entries the linear scan wins on
// every realistic toolchain — but to give future callers (config-loading
// large rule lists, schema generation) a recorded baseline so they reach
// for a different data structure with eyes open.

#include <nanobench.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <oran/core/capability.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using core::enum_name;
using core::enum_values;
using core::parse_enum;

// A deterministic mix of: every valid spelling once, three high-traffic
// spellings repeated, two unknown spellings. The map and the linear scan
// see byte-for-byte identical inputs.
[[nodiscard]] std::vector<std::string> make_lookup_inputs() {
  std::vector<std::string> inputs;
  for (const auto cap : enum_values<Capability>()) {
    inputs.emplace_back(enum_name(cap));
  }
  inputs.emplace_back("read_file");
  inputs.emplace_back("spawn_subprocess");
  inputs.emplace_back("runtime_loader");
  inputs.emplace_back("");
  inputs.emplace_back("not_a_capability");
  return inputs;
}

[[nodiscard]] std::unordered_map<std::string_view, Capability> make_map() {
  constexpr auto all = enum_values<Capability>();
  std::unordered_map<std::string_view, Capability> m;
  m.reserve(all.size());
  for (const auto cap : all) {
    m.emplace(enum_name(cap), cap);
  }
  return m;
}

}  // namespace

void register_capability_scenarios(ankerl::nanobench::Bench& bench) {
  static const std::vector<std::string> inputs = make_lookup_inputs();
  static const std::unordered_map<std::string_view, Capability> map = make_map();

  bench.run("core.capability_parse_linear", [&] {
    int hits = 0;
    for (const auto& key : inputs) {
      if (parse_enum<Capability>(key).has_value()) {
        ++hits;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });
  bench.run("core.capability_parse_unordered_map", [&] {
    int hits = 0;
    for (const auto& key : inputs) {
      if (map.contains(std::string_view{key})) {
        ++hits;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });
}

}  // namespace orangutan::bench
