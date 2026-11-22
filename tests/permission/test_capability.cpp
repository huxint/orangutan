#include <array>
#include <optional>
#include <span>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
using orangutan::core::Capability;
using perm::Mode;
using perm::Rule;
using perm::RuleSet;
using perm::Verdict;

TEST_CASE("every required capability needs authority", "[unit][permission][capability][policy]") {
  const std::array rules{Rule{.verdict = Verdict::allow, .tool_pattern = "*", .capability = Capability::read_file}};
  constexpr std::array required{Capability::read_file, Capability::write_file};

  const auto decision = perm::evaluate(rules, "CopyFile", required, Mode::strict);

  CHECK(decision.verdict == Verdict::deny);
}

TEST_CASE("a read grant does not bypass a write approval", "[unit][permission][capability][policy]") {
  const std::array rules{
      Rule{.verdict = Verdict::allow, .tool_pattern = "*", .capability = Capability::read_file},
      Rule{.verdict = Verdict::ask,
           .tool_pattern = "*",
           .capability = Capability::write_file,
           .replay_max = 2,
           .approval_ttl = std::chrono::seconds{60}},
  };
  constexpr std::array required{Capability::read_file, Capability::write_file};

  const auto decision = perm::evaluate(rules, "CopyFile", required, Mode::strict);

  CHECK(decision.verdict == Verdict::ask);
  CHECK(decision.reason.contains("write_file"));
  CHECK(decision.replay_max == 2);
  CHECK(decision.approval_ttl == std::chrono::seconds{60});
}

TEST_CASE("combined capabilities retain the narrowest approval limits", "[unit][permission][capability][policy]") {
  const std::array rules{
      Rule{.verdict = Verdict::ask,
           .tool_pattern = "*",
           .capability = Capability::read_file,
           .replay_max = 2,
           .approval_ttl = std::chrono::seconds{300}},
      Rule{.verdict = Verdict::ask,
           .tool_pattern = "*",
           .capability = Capability::write_file,
           .replay_max = 8,
           .approval_ttl = std::chrono::seconds{60}},
  };
  constexpr std::array required{Capability::read_file, Capability::write_file};

  const auto decision = perm::evaluate(rules, "CopyFile", required, Mode::strict);

  CHECK(decision.verdict == Verdict::ask);
  CHECK(decision.replay_max == 2);
  CHECK(decision.approval_ttl == std::chrono::seconds{60});
}

TEST_CASE("capability-bound rule fires when call requires the capability", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "File*",
      .capability = Capability::read_file,
  });

  constexpr std::array<Capability, 1> required{Capability::read_file};
  const auto decision = perm::evaluate(rs, "FileRead", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  REQUIRE(decision.reason.contains("capability=read_file"));
  REQUIRE(decision.reason.contains("File*"));
}

TEST_CASE("capability-bound rule does not fire when capability is missing", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "File*",
      .capability = Capability::read_file,
  });

  // The call only requires `write_file`; the rule's `read_file` scope filters
  // it out and we fall back to the mode default.
  constexpr std::array<Capability, 1> required{Capability::write_file};
  const auto decision = perm::evaluate(rs, "FileRead", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::deny);  // strict default
  REQUIRE(decision.reason.contains("default by mode=strict"));
}

TEST_CASE("a capability-less call skips capability-bound rules", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "File*",
      .capability = Capability::read_file,
  });

  // The capability-less overload behaves as if the caller passed `{}`; the
  // capability-bound rule does not fire and we fall back to the mode default.
  const auto decision = perm::evaluate(rs, "FileRead", Mode::permissive);
  REQUIRE(decision.verdict == Verdict::allow);  // permissive default
  REQUIRE(decision.reason.contains("default by mode=permissive"));
}

TEST_CASE("capability-bound deny outranks capability-bound allow at the same scope", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "*",
      .capability = Capability::spawn_subprocess,
  });
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });

  constexpr std::array<Capability, 2> required{
      Capability::spawn_subprocess,
      Capability::runtime_loader,
  };
  const auto decision = perm::evaluate(rs, "ShellExec", std::span<const Capability>{required}, Mode::permissive);
  REQUIRE(decision.verdict == Verdict::deny);
  REQUIRE(decision.reason.contains("capability=runtime_loader"));
}

TEST_CASE("capability mismatch falls through to next precedence pass", "[unit][permission][capability]") {
  RuleSet rs;
  // A deny scoped to a capability the call does not declare: should not fire.
  rs.push_back(Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = Capability::runtime_loader,
  });
  // An unscoped allow that should win.
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "ShellExec", .capability = std::nullopt});

  constexpr std::array<Capability, 1> required{Capability::spawn_subprocess};
  const auto decision = perm::evaluate(rs, "ShellExec", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  REQUIRE(decision.reason.contains("rule #1"));
  REQUIRE(!decision.reason.contains("capability="));
}

TEST_CASE("unscoped rule keeps matching when call passes capabilities", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{.verdict = Verdict::allow, .tool_pattern = "FileRead", .capability = std::nullopt});

  constexpr std::array<Capability, 2> required{
      Capability::read_file,
      Capability::write_file,
  };
  const auto decision = perm::evaluate(rs, "FileRead", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::allow);
  // Unscoped rules omit `capability=` from the reason.
  REQUIRE(!decision.reason.contains("capability="));
}

TEST_CASE("capability scope round-trips in reason for every verdict", "[unit][permission][capability]") {
  RuleSet rs;
  rs.push_back(Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "Memory*",
      .capability = Capability::write_file,
  });

  constexpr std::array<Capability, 1> required{Capability::write_file};
  const auto decision = perm::evaluate(rs, "MemoryRemember", std::span<const Capability>{required}, Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.reason.contains("ask"));
  REQUIRE(decision.reason.contains("Memory*"));
  REQUIRE(decision.reason.contains("capability=write_file"));
}
