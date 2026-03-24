#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orangutan {

using json = nlohmann::json;

enum class SubagentRuntimeOrigin {
    cli,
    channel,
    web,
};

enum class Role {
    User,
    Assistant,
};

[[nodiscard]]
constexpr std::string_view role_to_string(Role role) {
    switch (role) {
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
    }
    return "user";
}

[[nodiscard]]
constexpr Role role_from_string(std::string_view str) {
    if (str == "assistant") {
        return Role::Assistant;
    }
    return Role::User;
}

// A single content block in a message (text or tool related)
struct TextBlock {
    std::string text;
};

struct ToolUseBlock {
    std::string id;
    std::string name;
    json input;
};

struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

// A message in the conversation
struct Message {
    Role role = Role::User;
    std::vector<ContentBlock> content;

    // Convenience: create a simple text message
    static Message user_text(std::string_view text) {
        return {.role = Role::User, .content = {TextBlock{std::string{text}}}};
    }

    static Message assistant_text(std::string_view text) {
        return {.role = Role::Assistant, .content = {TextBlock{std::string{text}}}};
    }
};

// Tool definition sent to the API
struct ToolDef {
    std::string name;
    std::string description;
    json input_schema; // JSON Schema
};

// API response metadata
struct LLMResponse {
    std::string stop_reason; // "end_turn", "tool_use", etc.
    std::vector<ContentBlock> content;
};

// Serialize ContentBlock to JSON for API request
inline json content_block_to_json(const ContentBlock &block) {
    return std::visit(
        [](auto &&b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                return {{"type", "text"}, {"text", b.text}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
            } else {
                json j = {{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.content}};
                if (b.is_error) {
                    j["is_error"] = true;
                }
                return j;
            }
        },
        block);
}

// Serialize a Message to JSON
inline json message_to_json(const Message &msg) {
    json j;
    j["role"] = role_to_string(msg.role);
    j["content"] = json::array();
    for (const auto &block : msg.content) {
        j["content"].push_back(content_block_to_json(block));
    }
    return j;
}

} // namespace orangutan
