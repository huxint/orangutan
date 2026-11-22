#include <nanobench.h>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::default_rules;
using permission::Mode;
using permission::Rule;
using permission::RuleSet;
using permission::Verdict;

[[gnu::noinline]] RuleSet build_via_factory() {
  return default_rules(Mode::default_);
}

[[gnu::noinline]] RuleSet build_inline_default_baseline() {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "*", .capability = Capability::runtime_loader});
  rs.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "*", .capability = Capability::delete_path});
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "*", .capability = Capability::read_file});
  rs.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::write_file});
  rs.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::edit_file});
  rs.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::spawn_subprocess});
  rs.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "*", .capability = Capability::egress_http});
  return rs;
}

}  // namespace

void register_defaults_scenarios(ankerl::nanobench::Bench& bench) {
  bench.run("permission.defaults_build_default", [&] {
    auto rs = build_via_factory();
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
  bench.run("permission.defaults_hand_built_default", [&] {
    auto rs = build_inline_default_baseline();
    ankerl::nanobench::doNotOptimizeAway(rs);
  });
}

}  // namespace orangutan::bench
