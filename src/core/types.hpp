#pragma once

#include <spdlog/common.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orangutan {

    namespace base {
        using f32 = float;
        using f64 = double;

        using i8 = std::int8_t;
        using i16 = std::int16_t;
        using i32 = std::int32_t;
        using i64 = std::int64_t;

        using u8 = std::uint8_t;
        using u16 = std::uint16_t;
        using u32 = std::uint32_t;
        using u64 = std::uint64_t;
    } // namespace base

    using json = nlohmann::json;
    namespace fmt = spdlog::fmt_lib;

    enum class SubagentRuntimeOrigin : base::u8 {
        cli,
        channel,
        web,
    };

    enum class Role : base::u8 {
        user,
        assistant,
    };

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

    struct ThinkingBlock {
        std::string thinking;
    };

    using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

    // A message in the conversation
    struct Message {
        Role role = Role::user;
        std::vector<ContentBlock> content;

        // Convenience: create a simple text message
        static Message user_text(std::string_view text) {
            return {.role = Role::user, .content = {TextBlock{std::string{text}}}};
        }

        static Message assistant_text(std::string_view text) {
            return {.role = Role::assistant, .content = {TextBlock{std::string{text}}}};
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
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    json j = {{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.content}};
                    if (b.is_error) {
                        j["is_error"] = true;
                    }
                    return j;
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    return {{"type", "thinking"}, {"thinking", b.thinking}};
                }
            },
            block);
    }

    // Serialize a Message to JSON
    inline json message_to_json(const Message &msg) {
        json j;
        j["role"] = magic_enum::enum_name(msg.role);
        j["content"] = json::array();
        for (const auto &block : msg.content) {
            j["content"].push_back(content_block_to_json(block));
        }
        return j;
    }

} // namespace orangutan
