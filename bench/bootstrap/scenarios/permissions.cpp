#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include <nanobench.h>

#include <oran/bootstrap/permissions.hpp>
#include <oran/core/capability.hpp>

namespace orangutan::bench {

namespace {

using core::Capability;
using permission::Mode;
namespace cfg = orangutan::config;

[[nodiscard]] cfg::PermissionRuleConfig
rule(cfg::PermissionVerdict v, std::string pattern, std::optional<Capability> cap) {
  return cfg::PermissionRuleConfig{
      .verdict = v,
      .tool_pattern = std::move(pattern),
      .capability = cap,
  };
}

[[nodiscard]] cfg::PermissionsConfig make_global_fixture() {
  cfg::PermissionsConfig out;
  out.rules.reserve(8);
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "FileRead", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "FileEdit", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::allow, "*", Capability::egress_http));
  out.rules.push_back(rule(cfg::PermissionVerdict::deny, "*", Capability::runtime_loader));
  out.rules.push_back(rule(cfg::PermissionVerdict::deny, "ShellExec(rm:*)", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "FileWrite", std::nullopt));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "*", Capability::spawn_subprocess));
  out.rules.push_back(rule(cfg::PermissionVerdict::ask, "*", Capability::egress_websocket));
  return out;
}

}  // namespace

void register_permission_config(ankerl::nanobench::Bench& bench) {
  const auto global = make_global_fixture();

  bench.run("bootstrap.permissions_defaults_only", [] {
    const auto rules = bootstrap::materialize_permissions(Mode::default_, {});
    if (!rules) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(*rules);
  });
  bench.run("bootstrap.permissions_with_global", [&global] {
    const auto rules = bootstrap::materialize_permissions(Mode::default_, global.rules);
    if (!rules) {
      std::abort();
    }
    ankerl::nanobench::doNotOptimizeAway(*rules);
  });
}

}  // namespace orangutan::bench
