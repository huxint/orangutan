#include <nanobench.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using permission::InputPattern;
using permission::Mode;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[nodiscard]] RuleSet rule_set_with_pattern(const std::string& pattern) {
  auto pat = InputPattern::compile(pattern);
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::move(*pat),
  });
  return rs;
}

[[nodiscard]] RuleSet rule_set_without_pattern() {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "ShellExec",
  });
  return rs;
}

}  // namespace

void register_input_pattern_scenarios(ankerl::nanobench::Bench& b) {
  const auto patterned_rs = rule_set_with_pattern("^rm ");
  const auto plain_rs = rule_set_without_pattern();
  const auto matching_input = std::string_view{"rm -rf /tmp/scratch"};
  const auto non_matching_input = std::string_view{"ls -la /tmp/scratch"};

  b.run("permission.input_pattern_match", [&] {
    auto decision = permission::evaluate(patterned_rs, "ShellExec", matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });

  b.run("permission.input_pattern_miss", [&] {
    auto decision = permission::evaluate(patterned_rs, "ShellExec", non_matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });

  b.run("permission.no_input_pattern", [&] {
    auto decision = permission::evaluate(plain_rs, "ShellExec", matching_input, {}, Mode::permissive);
    ankerl::nanobench::doNotOptimizeAway(decision.verdict);
  });
}

}  // namespace orangutan::bench
