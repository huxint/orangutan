#pragma once

#include "automation/automation-types.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-dispatch.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::builtin::detail {

    using ParseError = std::string;

    [[nodiscard]]
    std::string resolve_agent_key(const ToolRuntimeContext &ctx);
    [[nodiscard]]
    std::string id_or_name(const nlohmann::json &request);
    [[nodiscard]]
    nlohmann::json normalize_automation_op_input(const nlohmann::json &input);
    [[nodiscard]]
    orangutan::tools::ToolDispatch::Response require_named_entity(const nlohmann::json &request, std::string_view missing_message, const auto &action) {
        const auto key = id_or_name(request);
        if (key.empty()) {
            return orangutan::tools::ToolDispatch::Response{.message = std::string(missing_message), .is_error = true};
        }
        return action(key);
    }

    [[nodiscard]]
    std::expected<automation::DeliveryPolicy, ParseError> parse_delivery_overlay(const nlohmann::json &input, const automation::DeliveryPolicy &base);
    [[nodiscard]]
    std::expected<std::optional<std::vector<automation::ActiveHourWindow>>, ParseError> parse_active_hours_overlay(const nlohmann::json &input);

} // namespace orangutan::builtin::detail
