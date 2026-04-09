#pragma once

#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    enum class approval_action : base::u8 {
        allow_once,
        always_allow,
        deny,
    };

    struct ParsedApprovalCallbackData {
        std::string request_id;
        approval_action action = approval_action::deny;
    };

    [[nodiscard]]
    std::string build_approval_callback_data(std::string_view request_id, approval_action action);

    [[nodiscard]]
    std::optional<ParsedApprovalCallbackData> parse_approval_callback_data(std::string_view data);

    [[nodiscard]]
    nlohmann::json build_approval_keyboard(std::string_view request_id, bool include_allow_always);

} // namespace orangutan::channel::qq
