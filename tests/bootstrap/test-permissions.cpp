#include <oran/bootstrap/permissions.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/permission.hpp>

namespace perm = orangutan::permission;
namespace cfg = orangutan::config;
namespace bootstrap = orangutan::bootstrap;
using orangutan::core::Capability;
using perm::default_rules;
using perm::Mode;
using perm::RuleSet;
using perm::Verdict;

namespace {

[[nodiscard]] cfg::PermissionRuleConfig allow(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::allow,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionRuleConfig deny(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionRuleConfig ask(std::string pattern, std::optional<Capability> cap = std::nullopt) {
  return cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] RuleSet require_materialized(Mode mode, const cfg::PermissionsConfig& global) {
  auto rs = bootstrap::materialize_permissions(mode, global.rules);
  REQUIRE(rs.has_value());
  return std::move(*rs);
}

[[nodiscard]] perm::Verdict
eval(const RuleSet& rs, std::string_view tool, std::span<const Capability> caps, Mode mode = Mode::default_) {
  return perm::evaluate(rs, tool, caps, mode).verdict;
}

[[nodiscard]] perm::Verdict eval_no_caps(const RuleSet& rs, std::string_view tool, Mode mode = Mode::default_) {
  return perm::evaluate(rs, tool, mode).verdict;
}

}  // namespace

TEST_CASE("materialize(empty) equals default_rules", "[unit][bootstrap][materialize]") {
  const auto rs = require_materialized(Mode::default_, cfg::PermissionsConfig{});
  REQUIRE(rs.size() == default_rules(Mode::default_).size());

  // Spot-check representative classifications.
  const std::array<Capability, 1> read{Capability::read_file};
  const std::array<Capability, 1> dangerous{Capability::runtime_loader};
  const std::array<Capability, 1> write{Capability::write_file};
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{read}) == Verdict::allow);
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{dangerous}) == Verdict::deny);
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{write}) == Verdict::ask);
}

TEST_CASE("materialize appends global config rules after defaults", "[unit][bootstrap][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("CustomTool"));

  const auto rs = require_materialized(Mode::default_, global);
  REQUIRE(rs.size() == default_rules(Mode::default_).size() + 1);

  // The CustomTool literal is now allowed even without a capability scope.
  REQUIRE(eval_no_caps(rs, "CustomTool") == Verdict::allow);

  // The defaults are still in place.
  const std::array<Capability, 1> read{Capability::read_file};
  REQUIRE(eval(rs, "anything", std::span<const Capability>{read}) == Verdict::allow);
}

TEST_CASE("materialize maps config verdicts one-to-one", "[unit][bootstrap][materialize]") {
  // Sandboxed mode's default is deny — so a non-matching call lands on deny,
  // letting us inspect every verdict mapping without baseline interference.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("allow.it"));
  global.rules.push_back(deny("deny.it"));
  global.rules.push_back(ask("ask.it"));

  const auto rs = require_materialized(Mode::sandboxed, global);
  REQUIRE(eval_no_caps(rs, "allow.it", Mode::sandboxed) == Verdict::allow);
  REQUIRE(eval_no_caps(rs, "deny.it", Mode::sandboxed) == Verdict::deny);
  REQUIRE(eval_no_caps(rs, "ask.it", Mode::sandboxed) == Verdict::ask);
}

TEST_CASE("materialize preserves capability scope on config-side rules", "[unit][bootstrap][materialize]") {
  // Strict mode defaults to deny on no-match; ship an empty Defaults baseline
  // so only the config rule fires.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("*", Capability::egress_websocket));

  const auto rs = require_materialized(Mode::strict, global);
  const std::array<Capability, 1> net{Capability::egress_websocket};
  const std::array<Capability, 1> file{Capability::read_file};

  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{net}, Mode::strict) == Verdict::allow);
  // Without the required capability the rule misses and the strict-mode
  // default verdict (deny) takes over.
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{file}, Mode::strict) == Verdict::deny);
}

TEST_CASE("explicit deny in any layer outranks allow in any other layer", "[unit][bootstrap][materialize]") {
  // default_rules(permissive) already denies runtime_loader; ship a
  // global allow for the same capability and confirm the deny wins.
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("*", Capability::runtime_loader));

  const auto rs = require_materialized(Mode::permissive, global);
  const std::array<Capability, 1> loader{Capability::runtime_loader};
  REQUIRE(eval(rs, "any.tool", std::span<const Capability>{loader}, Mode::permissive) == Verdict::deny);
}

TEST_CASE("materialize preserves intra-layer rule order", "[unit][bootstrap][materialize]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(allow("first"));
  global.rules.push_back(allow("second"));

  const auto rs = require_materialized(Mode::strict, global);

  // Reason text encodes the rule index; the first matching rule wins at the
  // same verdict, so the index distinguishes order.
  const auto first = perm::evaluate(rs, "first", Mode::strict);
  const auto second = perm::evaluate(rs, "second", Mode::strict);
  REQUIRE(first.verdict == Verdict::allow);
  REQUIRE(second.verdict == Verdict::allow);
  REQUIRE(first.reason.contains("first"));
  REQUIRE(second.reason.contains("second"));
}

TEST_CASE("materialize compiles config-side input_pattern into the runtime Rule",
          "[unit][bootstrap][materialize][input_pattern]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::string{"^rm "},
  });

  const auto rs = require_materialized(Mode::permissive, global);
  // Input matches the regex -> rule fires.
  const auto blocked = perm::evaluate(rs, "ShellExec", "rm -rf /tmp", {}, Mode::permissive);
  REQUIRE(blocked.verdict == Verdict::deny);
  REQUIRE(blocked.reason.contains("input=~"));
  REQUIRE(blocked.reason.contains("^rm "));

  // Non-matching input -> falls through.
  const auto allowed = perm::evaluate(rs, "ShellExec", "ls -la", {}, Mode::permissive);
  REQUIRE(allowed.verdict == Verdict::allow);
}

TEST_CASE("materialize surfaces re2 compile failures on input_pattern",
          "[unit][bootstrap][materialize][input_pattern]") {
  // Bypass the config-side validator by feeding an invalid pattern directly
  // into `PermissionRuleConfig`. The materialize() compile attempt should
  // produce `ErrorKind::invalid_argument`.
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::deny,
      .tool_pattern = "ShellExec",
      .input_pattern = std::string{"[unclosed"},
  });

  auto rs = bootstrap::materialize_permissions(Mode::permissive, global.rules);
  REQUIRE_FALSE(rs.has_value());
  REQUIRE(rs.error().kind() == orangutan::core::ErrorKind::invalid_argument);
}

TEST_CASE("materialize forwards replay_max + approval_ttl_seconds from config into the Rule",
          "[unit][bootstrap][materialize][approval_policy]") {
  cfg::PermissionsConfig global;
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = "FileWrite",
      .replay_max = std::uint32_t{4},
      .approval_ttl_seconds = std::int64_t{120},
  });

  const auto rs = require_materialized(Mode::strict, global);
  const auto decision = perm::evaluate(rs, "FileWrite", Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 4);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{120});
}

TEST_CASE("materialize keeps Rule defaults when config omits replay_max / approval_ttl_seconds",
          "[unit][bootstrap][materialize][approval_policy]") {
  cfg::PermissionsConfig global;
  // Both optional fields unset — operator omitted them.
  global.rules.push_back(cfg::PermissionRuleConfig{
      .verdict = cfg::PermissionVerdict::ask,
      .tool_pattern = "ShellExec",
  });

  const auto rs = require_materialized(Mode::strict, global);
  const auto decision = perm::evaluate(rs, "ShellExec", Mode::strict);
  REQUIRE(decision.verdict == Verdict::ask);
  REQUIRE(decision.replay_max == 8);
  REQUIRE(decision.approval_ttl == std::chrono::seconds{3600});
}
