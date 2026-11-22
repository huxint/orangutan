#pragma once

#include <span>

#include <oran/config/config.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::bootstrap {

/// Compile owned rules in baseline then configuration order. Workspace settings
/// are separate; invalid input patterns return an error without a partial policy.
[[nodiscard]] core::Result<permission::RuleSet>
materialize_permissions(permission::Mode mode, std::span<const config::PermissionRuleConfig> global);

}  // namespace orangutan::bootstrap
