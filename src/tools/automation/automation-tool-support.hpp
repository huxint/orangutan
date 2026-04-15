#pragma once

#include "automation/model.hpp"
#include "automation/parser.hpp"
#include "automation/repository.hpp"
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
    std::string resolve_query_agent_key(const ToolRuntimeContext *ctx, const nlohmann::json &request);
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
    std::expected<automation::TriggerDefinition, ParseError> parse_trigger_value(const nlohmann::json &input);
    [[nodiscard]]
    std::expected<std::vector<std::string>, ParseError> parse_tags_value(const nlohmann::json &input, const std::vector<std::string> &base);
    [[nodiscard]]
    std::expected<automation::Automation, ParseError> parse_create_request(const nlohmann::json &request, std::string_view agent_key);
    [[nodiscard]]
    std::expected<automation::Automation, ParseError> apply_update_request(const nlohmann::json &request, const automation::Automation &base, std::string_view agent_key);

} // namespace orangutan::builtin::detail
