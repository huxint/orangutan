#pragma once

#include "permissions/permission-types.hpp"

#include <optional>
#include <string_view>

namespace orangutan::permissions {

    bool is_protected_path(std::string_view path);
    bool is_write_operation(const ToolUse &call);
    std::optional<PermissionDecision> check_safety(const ToolUse &call);

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::check_safety;
    using permissions::is_protected_path;

} // namespace orangutan
