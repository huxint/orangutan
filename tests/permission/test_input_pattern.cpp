// tests/permission/test_input_pattern.cpp — runtime regex coverage for
// `permission::InputPattern` and `Rule::input_pattern` matching.

#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <oran/permission.hpp>

namespace perm = orangutan::permission;
using perm::InputPattern;
using perm::Mode;
using perm::Rule;
using perm::RuleSet;
using perm::Verdict;

TEST_CASE("InputPattern compiles a valid regex", "[unit][permission][input_pattern]") {
  auto result = InputPattern::compile("^rm ");
  REQUIRE(result.has_value());
  REQUIRE(result->pattern() == "^rm ");
}

TEST_CASE("InputPattern compile rejects invalid regex with re2 error attached", "[unit][permission][input_pattern]") {
  // An unmatched `[` is the canonical re2 syntax error.
  auto result = InputPattern::compile("[unclosed");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == orangutan::core::ErrorKind::invalid_argument);
  // The re2 error message is exposed under the `regex_error` context key.
  const auto ctx = result.error().context();
  bool has_regex_error = false;
  for (const auto& [key, value] : ctx) {
    if (key == "regex_error" && !value.empty()) {
      has_regex_error = true;
      break;
    }
  }
  REQUIRE(has_regex_error);
}

TEST_CASE("InputPattern::matches uses re2 partial-match semantics", "[unit][permission][input_pattern]") {
  auto pat = InputPattern::compile("rm ");
  REQUIRE(pat.has_value());
  REQUIRE(pat->matches("rm -rf /"));
  REQUIRE(pat->matches("/bin/rm -rf"));
  REQUIRE_FALSE(pat->matches("ls -la"));
}

TEST_CASE("InputPattern matches anchored expressions exactly", "[unit][permission][input_pattern]") {
  auto pat = InputPattern::compile("^git push");
  REQUIRE(pat.has_value());
  REQUIRE(pat->matches("git push origin main"));
  REQUIRE_FALSE(pat->matches("sudo git push origin main"));
}

TEST_CASE("InputPattern is move-constructible", "[unit][permission][input_pattern]") {
  auto first = InputPattern::compile("hello");
  REQUIRE(first.has_value());

  auto moved = std::move(*first);
  REQUIRE(moved.matches("hello world"));
  REQUIRE(moved.pattern() == "hello");
}

TEST_CASE("InputPattern equality compares the source pattern", "[unit][permission][input_pattern]") {
  // Two equivalent NFAs spelt differently are *not* equal — equality is on
  // the source string by design.
  auto a = InputPattern::compile("a|b");
  auto b = InputPattern::compile("a|b");
  auto c = InputPattern::compile("(a|b)");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(c.has_value());
  REQUIRE(*a == *b);
  REQUIRE_FALSE(*a == *c);
}

TEST_CASE("Rule with input_pattern fires only when the pattern matches",
          "[unit][permission][rule_set][input_pattern]") {
  auto pat = InputPattern::compile("^rm ");
  REQUIRE(pat.has_value());

  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::move(*pat),
  });

  // Matching input -> deny fires.
  const auto blocked = perm::evaluate(rs, "ShellExec", "rm -rf /", {}, Mode::permissive);
  REQUIRE(blocked.verdict == Verdict::deny);
  REQUIRE(blocked.reason.contains("rule #0"));
  REQUIRE(blocked.reason.contains("input=~"));
  REQUIRE(blocked.reason.contains("^rm "));

  // Non-matching input -> rule misses, mode default applies.
  const auto allowed = perm::evaluate(rs, "ShellExec", "ls -la", {}, Mode::permissive);
  REQUIRE(allowed.verdict == Verdict::allow);
  REQUIRE(allowed.reason.contains("default by mode=permissive"));
}

TEST_CASE("No-input evaluate skips input-pattern rules unless the pattern accepts \"\"",
          "[unit][permission][rule_set][input_pattern]") {
  // A pattern that only matches non-empty inputs should never fire when the
  // caller used the no-input evaluate overload.
  auto strict = InputPattern::compile("rm ");
  REQUIRE(strict.has_value());

  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::move(*strict),
  });

  REQUIRE(perm::evaluate(rs, "ShellExec", Mode::permissive).verdict == Verdict::allow);

  // A pattern that accepts the empty string (re2 .* on "") DOES fire on the
  // no-input path — the semantics are honest about what "empty input" means.
  auto loose = InputPattern::compile(".*");
  REQUIRE(loose.has_value());

  RuleSet rs2;
  rs2.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::move(*loose),
  });

  REQUIRE(perm::evaluate(rs2, "ShellExec", Mode::permissive).verdict == Verdict::deny);
}

TEST_CASE("Capability + input_pattern compose on a single rule", "[unit][permission][rule_set][input_pattern]") {
  auto pat = InputPattern::compile("DROP TABLE");
  REQUIRE(pat.has_value());

  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = orangutan::core::Capability::write_file,
      .input_pattern = std::move(*pat),
  });

  const std::array caps_write{orangutan::core::Capability::write_file};
  const std::array caps_read{orangutan::core::Capability::egress_http};

  // Both axes match -> deny.
  REQUIRE(perm::evaluate(rs,
                         "MemoryExec",
                         "UPDATE x; DROP TABLE y;",
                         std::span<const orangutan::core::Capability>{caps_write},
                         Mode::permissive)
              .verdict == Verdict::deny);

  // Input doesn't match -> rule misses.
  REQUIRE(perm::evaluate(rs,
                         "MemoryExec",
                         "SELECT 1",
                         std::span<const orangutan::core::Capability>{caps_write},
                         Mode::permissive)
              .verdict == Verdict::allow);

  // Capability doesn't match -> rule misses (input alone is not enough).
  REQUIRE(perm::evaluate(rs,
                         "MemoryExec",
                         "UPDATE x; DROP TABLE y;",
                         std::span<const orangutan::core::Capability>{caps_read},
                         Mode::permissive)
              .verdict == Verdict::allow);
}
