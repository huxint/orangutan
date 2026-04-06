#pragma once

#include "permissions/permission-types.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace orangutan::permissions {

    using ToolPermissionChecker = std::function<PermissionResult(const ToolUse &call, const ToolPermissionContext &ctx)>;

    using IsReadOnlyChecker = std::function<bool()>;

    PermissionDecision evaluate_permission(const ToolUse &call, const ToolPermissionContext &ctx,
                                           const ToolPermissionChecker &tool_checker = {},
                                           const IsReadOnlyChecker &is_read_only = {});

    PermissionDecision apply_post_processing(PermissionDecision decision, permission_mode mode);

    std::optional<PermissionRule> find_matching_rule(std::string_view tool_name, std::string_view content,
                                                     const std::vector<PermissionRule> &rules);

    std::optional<PermissionDecision> evaluate_mode(permission_mode mode, const ToolUse &call,
                                                     bool is_read_only, bool is_file_tool_in_workspace = false);

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::apply_post_processing;
    using permissions::evaluate_permission;

} // namespace orangutan
