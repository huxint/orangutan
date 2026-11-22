#include <oran/bootstrap/permissions.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <utility>

#include <oran/core/enum_names.hpp>
#include <oran/permission/defaults.hpp>
#include <oran/permission/input_pattern.hpp>

namespace orangutan::bootstrap {

core::Result<permission::RuleSet> materialize_permissions(permission::Mode mode,
                                                          std::span<const config::PermissionRuleConfig> global) {
  auto rules = permission::default_rules(mode);
  rules.reserve(rules.size() + global.size());
  for (const auto& rule : global) {
    auto input_pattern = std::optional<permission::InputPattern>{};
    if (rule.input_pattern.has_value()) {
      auto compiled = permission::InputPattern::compile(*rule.input_pattern);
      if (!compiled) {
        return std::unexpected(std::move(compiled).error());
      }
      input_pattern = std::move(*compiled);
    }
    const auto verdict =
        core::parse_enum<permission::Verdict>(core::enum_name(rule.verdict)).value_or(permission::Verdict::deny);
    permission::Rule runtime_rule{
        .verdict = verdict,
        .tool_pattern = rule.tool_pattern,
        .capability = rule.capability,
        .input_pattern = std::move(input_pattern),
    };
    if (rule.replay_max.has_value()) {
      runtime_rule.replay_max = *rule.replay_max;
    }
    if (rule.approval_ttl_seconds.has_value()) {
      runtime_rule.approval_ttl = std::chrono::seconds{*rule.approval_ttl_seconds};
    }
    rules.push_back(std::move(runtime_rule));
  }
  return rules;
}

}  // namespace orangutan::bootstrap
