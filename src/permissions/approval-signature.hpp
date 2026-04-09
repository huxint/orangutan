#pragma once

#include "permissions/permission-types.hpp"

#include <optional>
#include <string>

namespace orangutan {
    struct ToolUse;
}

namespace orangutan::permissions {

    struct ApprovalSignature {
        std::optional<RuleContent> content;
        bool always_allow_eligible = false;
        std::string downgrade_reason;
    };

    [[nodiscard]]
    ApprovalSignature derive_approval_signature(const ToolUse &call);

    [[nodiscard]]
    std::string approval_match_content(const ToolUse &call);

    [[nodiscard]]
    std::optional<PermissionRule> make_session_allow_rule(const ToolUse &call);

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::ApprovalSignature;
    using permissions::approval_match_content;
    using permissions::derive_approval_signature;
    using permissions::make_session_allow_rule;

} // namespace orangutan
