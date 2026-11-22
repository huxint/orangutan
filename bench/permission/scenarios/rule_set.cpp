#include <nanobench.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::glob_match;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[nodiscard]] std::vector<Rule> make_fixture() {
  std::vector<Rule> rules;
  rules.reserve(16);
  for (int i = 0; i < 6; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::allow,
        .tool_pattern = "FileRead" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::ask,
        .tool_pattern = "FileWrite" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  for (int i = 0; i < 4; ++i) {
    rules.push_back(Rule{
        .verdict = Verdict::deny,
        .tool_pattern = "ShellRm" + std::to_string(i),
        .capability = std::nullopt,
    });
  }
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "Shell*", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "ShellRm", .capability = std::nullopt});
  return rules;
}

[[gnu::noinline]] permission::Verdict linear_first_match(const std::vector<Rule>& rules,
                                                         std::string_view tool_name) noexcept {
  const auto it =
      std::ranges::find_if(rules, [&](const Rule& rule) noexcept { return glob_match(rule.tool_pattern, tool_name); });
  if (it == rules.end()) {
    return permission::Verdict::deny;
  }
  return it->verdict;
}

// A 16-rule fixture with capability scopes. Half the rules are capability-
// bound (mirroring the design-doc capability-aware-gating example); the
// other half are unscoped, so the precedence walk still does realistic work
// across both shapes.
[[nodiscard]] std::vector<Rule> make_capability_fixture() {
  std::vector<Rule> rules;
  rules.reserve(16);
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "File*",
      .capability = Capability::read_file,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "File*",
      .capability = Capability::write_file,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "Shell*",
      .capability = Capability::spawn_subprocess,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "Net*",
      .capability = Capability::egress_http,
  });
  rules.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "Network*",
      .capability = Capability::egress_http,
  });
  rules.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "File*",
      .capability = Capability::write_file,
  });
  rules.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "Shell*",
      .capability = Capability::spawn_subprocess,
  });
  rules.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "ping", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "version", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "ClockNow", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "CustomAction", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "AgentRun", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "ShellRm", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "ShellForkBomb", .capability = std::nullopt});
  rules.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "Process*", .capability = std::nullopt});
  return rules;
}

}  // namespace

void register_rule_set_scenarios(ankerl::nanobench::Bench& bench) {
  static const std::vector<Rule> fixture = make_fixture();
  static const RuleSet rule_set = [] {
    RuleSet rs;
    auto rules = make_fixture();
    for (auto& rule : rules) {
      rs.push_back(std::move(rule));
    }
    return rs;
  }();
  // Pick a tool name that needs all three precedence passes (deny matches
  // late in the list, allow matches in the wildcard rule, ask never fires).
  static const std::string tool_name = "ShellRm";

  bench.run("permission.rule_set_evaluate", [&] {
    auto decision = permission::evaluate(rule_set, tool_name, permission::Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
  bench.run("permission.linear_find_if", [&] {
    auto verdict = linear_first_match(fixture, tool_name);
    ankerl::nanobench::doNotOptimizeAway(verdict);
  });

  static const RuleSet cap_rule_set = [] {
    RuleSet rs;
    auto rules = make_capability_fixture();
    for (auto& rule : rules) {
      rs.push_back(std::move(rule));
    }
    return rs;
  }();

  static const std::array required_for_match{Capability::spawn_subprocess};
  static const std::array<Capability, 2> required_for_miss{
      // Neither value appears on any capability-bound rule in the fixture, so
      // every capability-bound rule filters out and the call falls through to
      // the unscoped rules (none of which match this tool name) and finally
      // to the mode default.
      Capability::signal_subprocess,
      Capability::external_mcp,
  };
  static const std::string capability_tool = "ShellExec";

  bench.run("permission.rule_set_capability_match", [&] {
    auto decision = permission::evaluate(cap_rule_set,
                                         capability_tool,
                                         std::span<const Capability>{required_for_match},
                                         permission::Mode::strict);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
  bench.run("permission.rule_set_capability_miss", [&] {
    auto decision = permission::evaluate(cap_rule_set,
                                         capability_tool,
                                         std::span<const Capability>{required_for_miss},
                                         permission::Mode::strict);
    ankerl::nanobench::doNotOptimizeAway(decision);
  });
}

}  // namespace orangutan::bench
