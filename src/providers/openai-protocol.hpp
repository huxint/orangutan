#pragma once

#include "types/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace orangutan::providers::detail {

    struct OpenAiToolCallState {
        std::string id;
        std::string name;
        std::string arguments;
        bool announced = false;
    };

    void merge_chat_completions_tool_call_delta(OpenAiToolCallState &state, const nlohmann::json &delta);

    [[nodiscard]]
    std::optional<ToolUse> finalize_openai_tool_call(const OpenAiToolCallState &state, std::string_view source);

    [[nodiscard]]
    bool is_valid_openai_tool_use(const ToolUse &tool_use);

    [[nodiscard]]
    std::optional<nlohmann::json> serialize_openai_assistant_message(const Message &message);

} // namespace orangutan::providers::detail
