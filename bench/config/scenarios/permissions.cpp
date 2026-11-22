// Compare empty and populated permission blocks, including input-pattern
// validation. Bootstrap separately compiles the parsed values into runtime rules.

#include <nanobench.h>

#include <cstdlib>
#include <string_view>

#include <oran/config.hpp>

namespace orangutan::bench {
namespace config = orangutan::config;

namespace {

constexpr auto kEmptyPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {}
}
)json"};

constexpr auto kTypedPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {
    "allow": [
      {"tool_pattern": "FileRead"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "read_file"},
      {"tool_pattern": "*", "capability": "list_directory"},
      {"tool_pattern": "*", "capability": "egress_http"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"},
      {"tool_pattern": "*", "capability": "delete_path"},
      {"tool_pattern": "ShellExec(rm:*)"}
    ],
    "ask": [
      {"tool_pattern": "FileWrite"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "write_file"},
      {"tool_pattern": "*", "capability": "edit_file"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"},
      {"tool_pattern": "*", "capability": "signal_subprocess"},
      {"tool_pattern": "*", "capability": "egress_websocket"},
      {"tool_pattern": "*", "capability": "external_mcp"}
    ]
  }
}
)json"};

constexpr auto kInputPatternPermissionsConfig = std::string_view{R"json(
{
  "runtime": {"workers": 1},
  "permissions": {
    "allow": [
      {"tool_pattern": "FileRead"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "read_file"},
      {"tool_pattern": "*", "capability": "list_directory"},
      {"tool_pattern": "*", "capability": "egress_http"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"},
      {"tool_pattern": "*", "capability": "delete_path"},
      {"tool_pattern": "ShellExec", "input_pattern": "^rm -rf"},
      {"tool_pattern": "ShellExec", "input_pattern": "^git push"},
      {"tool_pattern": "ShellExec", "input_pattern": "^sudo"},
      {"tool_pattern": "ShellExec", "input_pattern": "; *rm "}
    ],
    "ask": [
      {"tool_pattern": "FileWrite"},
      {"tool_pattern": "FileEdit"},
      {"tool_pattern": "*", "capability": "write_file"},
      {"tool_pattern": "*", "capability": "edit_file"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"}
    ]
  }
}
)json"};

[[gnu::noinline]] std::size_t parse_empty_permissions() {
  auto parsed = config::Config::parse(kEmptyPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size();
}

[[gnu::noinline]] std::size_t parse_typed_permissions() {
  auto parsed = config::Config::parse(kTypedPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size();
}

[[gnu::noinline]] std::size_t parse_input_pattern_permissions() {
  auto parsed = config::Config::parse(kInputPatternPermissionsConfig);
  if (!parsed) {
    std::abort();
  }
  return parsed->permissions().rules.size();
}

}  // namespace

void register_permissions(ankerl::nanobench::Bench& bench) {
  bench.run("config.parse_permissions_empty", [] {
    const auto value = parse_empty_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("config.parse_permissions_typed", [] {
    const auto value = parse_typed_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("config.parse_permissions_with_input_patterns", [] {
    const auto value = parse_input_pattern_permissions();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
