#pragma once

#include "permissions/permission-types.hpp"

#include <string_view>

namespace orangutan::permissions {

    PermissionRule parse_permission_rule(std::string_view rule_str, PermissionBehavior behavior, PermissionRuleSource source);

    bool matches_rule(const PermissionRule &rule, std::string_view tool_name, std::string_view content = {});

    bool matches_prefix(std::string_view pattern, std::string_view input);
    bool matches_wildcard(std::string_view pattern, std::string_view input);

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::matches_prefix;
    using permissions::matches_rule;
    using permissions::matches_wildcard;
    using permissions::parse_permission_rule;

} // namespace orangutan
