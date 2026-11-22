// tests/permission/test_rule_set.cpp — `RuleSet` evaluator + glob matcher.

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/permission.hpp>

using namespace std::string_view_literals;

namespace perm = orangutan::permission;
namespace core = orangutan::core;
using perm::Mode;
using perm::Rule;
using perm::RuleSet;
using perm::Verdict;

TEST_CASE("glob_match handles literal and wildcard patterns", "[unit][permission][glob]") {
  REQUIRE(perm::glob_match("FileRead"sv, "FileRead"sv));
  REQUIRE_FALSE(perm::glob_match("FileRead"sv, "FileWrite"sv));
  REQUIRE(perm::glob_match("File*"sv, "FileRead"sv));
  REQUIRE(perm::glob_match("File*"sv, "FileWrite"sv));
  REQUIRE(perm::glob_match("File*"sv, "File"sv));
  REQUIRE_FALSE(perm::glob_match("File*"sv, "ShellExec"sv));
  REQUIRE(perm::glob_match("*"sv, ""sv));
  REQUIRE(perm::glob_match("*"sv, "anything"sv));
  REQUIRE(perm::glob_match("a*c"sv, "ac"sv));
  REQUIRE(perm::glob_match("a*c"sv, "abc"sv));
  REQUIRE(perm::glob_match("a*c"sv, "abbbbbc"sv));
  REQUIRE_FALSE(perm::glob_match("a*c"sv, "ab"sv));
  // Multiple wildcards: backtracking required.
  REQUIRE(perm::glob_match("*Tool*Read*"sv, "PreToolReadPost"sv));
  REQUIRE_FALSE(perm::glob_match("*Tool*Read*"sv, "PreToolWritePost"sv));
}

TEST_CASE("empty RuleSet falls back to mode default", "[unit][permission][rule_set]") {
  RuleSet rs;
  REQUIRE(rs.size() == 0);
  REQUIRE(perm::evaluate(rs, "FileRead", Mode::strict).verdict == Verdict::deny);
  REQUIRE(perm::evaluate(rs, "FileRead", Mode::sandboxed).verdict == Verdict::deny);
  REQUIRE(perm::evaluate(rs, "FileRead", Mode::default_).verdict == Verdict::ask);
  REQUIRE(perm::evaluate(rs, "FileRead", Mode::permissive).verdict == Verdict::allow);
}

TEST_CASE("RuleSet allow rules match by exact name and glob", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "FileRead", .capability = std::nullopt});
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "Shell*", .capability = std::nullopt});

  REQUIRE(perm::evaluate(rs, "FileRead", Mode::strict).verdict == Verdict::allow);
  REQUIRE(perm::evaluate(rs, "ShellExec", Mode::strict).verdict == Verdict::allow);
  REQUIRE(perm::evaluate(rs, "ShellLs", Mode::strict).verdict == Verdict::allow);
  // Unmatched in strict mode -> deny.
  REQUIRE(perm::evaluate(rs, "NetworkFetch", Mode::strict).verdict == Verdict::deny);
}

TEST_CASE("RuleSet deny wins over allow regardless of order", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "Shell*", .capability = std::nullopt});
  rs.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "ShellRm", .capability = std::nullopt});

  auto blocked = perm::evaluate(rs, "ShellRm", Mode::permissive);
  REQUIRE(blocked.verdict == Verdict::deny);
  REQUIRE(blocked.reason.contains("deny"));

  auto allowed = perm::evaluate(rs, "ShellLs", Mode::permissive);
  REQUIRE(allowed.verdict == Verdict::allow);
  REQUIRE(allowed.reason.contains("allow"));
}

TEST_CASE("RuleSet ask fires when no allow/deny matches", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "FileRead", .capability = std::nullopt});
  rs.push_back(Rule{.verdict = Verdict::ask, .tool_pattern = "FileWrite", .capability = std::nullopt});

  auto write = perm::evaluate(rs, "FileWrite", Mode::strict);
  REQUIRE(write.verdict == Verdict::ask);
  REQUIRE(write.reason.contains("ask"));
}

TEST_CASE("RuleSet returns the index of the first matching rule in its reason", "[unit][permission][rule_set]") {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "x", .capability = std::nullopt});       // 0
  rs.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "ShellRm", .capability = std::nullopt});  // 1
  rs.push_back(Rule{.verdict = Verdict::deny, .tool_pattern = "Shell*", .capability = std::nullopt});   // 2

  auto decision = perm::evaluate(rs, "ShellRm", Mode::permissive);
  REQUIRE(decision.verdict == Verdict::deny);
  REQUIRE(decision.reason.contains("rule #1"));
}

TEST_CASE("Verdict and Mode have stable string mappings", "[unit][permission]") {
  REQUIRE(core::enum_name(Verdict::allow) == "allow");
  REQUIRE(core::enum_name(Verdict::deny) == "deny");
  REQUIRE(core::enum_name(Verdict::ask) == "ask");
  REQUIRE(core::enum_name(Mode::strict) == "strict");
  REQUIRE(core::enum_name(Mode::default_) == "default");
  REQUIRE(core::enum_name(Mode::permissive) == "permissive");
  REQUIRE(core::enum_name(Mode::sandboxed) == "sandboxed");
}

TEST_CASE("Rule defaults match the design-doc approval-policy baseline",
          "[unit][permission][rule_set][approval_policy]") {
  const Rule rule{.tool_pattern = "*"};
  REQUIRE(rule.replay_max == 8);
  REQUIRE(rule.approval_ttl == std::chrono::seconds{3600});
}

TEST_CASE("permission::evaluate forwards rule replay_max / approval_ttl onto the Decision",
          "[unit][permission][rule_set][approval_policy]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "FileWrite",
      .replay_max = 2,
      .approval_ttl = std::chrono::seconds{60},
  });
  const auto decision = perm::evaluate(rs, "FileWrite", Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 2);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{60});
}

TEST_CASE("permission::evaluate falls back to Decision defaults on the mode-default branch",
          "[unit][permission][rule_set][approval_policy]") {
  RuleSet rs;
  const auto decision = perm::evaluate(rs, "FileWrite", Mode::default_);
  // No rule fired — Decision keeps the design-doc baseline (8 / 1h).
  REQUIRE(decision.replay_max == 8);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{3600});
}

TEST_CASE("policy intersection preserves the more restrictive verdict", "[unit][permission][collaboration]") {
  const auto cases = std::array{
      std::array{Verdict::allow, Verdict::allow, Verdict::allow},
      std::array{Verdict::allow, Verdict::ask, Verdict::ask},
      std::array{Verdict::allow, Verdict::deny, Verdict::deny},
      std::array{Verdict::ask, Verdict::allow, Verdict::ask},
      std::array{Verdict::ask, Verdict::ask, Verdict::ask},
      std::array{Verdict::ask, Verdict::deny, Verdict::deny},
      std::array{Verdict::deny, Verdict::allow, Verdict::deny},
      std::array{Verdict::deny, Verdict::ask, Verdict::deny},
      std::array{Verdict::deny, Verdict::deny, Verdict::deny},
  };
  for (const auto& row : cases) {
    CAPTURE(row[0], row[1]);
    auto decision = perm::intersect(perm::Decision{.verdict = row[0], .reason = "parent"},
                                    perm::Decision{.verdict = row[1], .reason = "child"});
    REQUIRE(decision.verdict == row[2]);
  }
}

TEST_CASE("policy intersection combines approval replay and expiry limits", "[unit][permission][collaboration]") {
  const auto parent = perm::Decision{.verdict = Verdict::ask,
                                     .reason = "parent",
                                     .replay_max = 2,
                                     .approval_ttl = std::chrono::seconds{90}};
  const auto child = perm::Decision{.verdict = Verdict::ask,
                                    .reason = "child",
                                    .replay_max = 8,
                                    .approval_ttl = std::chrono::seconds{20}};

  const auto decision = perm::intersect(parent, child);
  const auto reversed = perm::intersect(child, parent);

  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 2);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{20});
  REQUIRE(reversed.replay_max == 2);
  REQUIRE(reversed.approval_ttl == std::chrono::seconds{20});
}
