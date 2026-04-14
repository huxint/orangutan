#pragma once

#include "types/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace orangutan::providers::protocols::openai {

    struct ToolCallState {
        std::string id;
        std::string name;
        std::string arguments;
        bool announced = false;
    };

    void merge_tool_call_delta(ToolCallState &state, const nlohmann::json &delta);

    [[nodiscard]]
    std::optional<ToolUse> finalize_tool_call(const ToolCallState &state, std::string_view source);

    [[nodiscard]]
    std::optional<nlohmann::json> serialize_assistant_message(const Message &message);

    [[nodiscard]]
    std::optional<nlohmann::json> serialize_message(const Message &message);

    void append_chat_history_message(nlohmann::json &chat_messages, const Message &message);

    void append_responses_history_message(nlohmann::json &responses_input, const Message &message);

    [[nodiscard]]
    nlohmann::json chat_tool_to_json(const ToolDef &tool);

    [[nodiscard]]
    nlohmann::json response_tool_to_json(const ToolDef &tool);

} // namespace orangutan::providers::protocols::openai
