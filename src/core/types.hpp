#pragma once

#include <spdlog/common.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace orangutan {
    // base type
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

        enum class role : base::u8 {
            user,
            assistant,
        };

        enum class origin : base::u8 {
            cli,
            channel,
            web,
        };
    } // namespace base

    // Alias of a third-party library dependency
    namespace deps {
        using json = nlohmann::json;

        namespace magic_enum = ::magic_enum;
        namespace log = spdlog;
        namespace fmt = spdlog::fmt_lib;
    } // namespace deps

    namespace core::block {
        // A single content block in a message (text or tool related)
        struct Text {
            std::string text;
        };

        struct Thinking {
            std::string thinking;
        };

        struct ToolUse {
            std::string id;
            std::string name;
            deps::json input;
        };

        struct ToolResult {
            std::string tool_use_id;
            std::string content;
            bool is_error = false;
        };

        using text = Text;
        using thinking = Thinking;
        using tool_use = ToolUse;
        using tool_result = ToolResult;

        using content = std::variant<text, thinking, tool_use, tool_result>;
    } // namespace core::block

    namespace tools {
        // Tool definition sent to the API
        struct ToolDef {
            std::string name;
            std::string description;
            deps::json input_schema; // JSON Schema
        };
    } // namespace tools

    namespace llm {
        // API response metadata
        struct LLMResponse {
            std::string stop_reason; // "end_turn", "tool_use", etc.
            std::vector<core::block::content> content;
        };
    } // namespace llm

} // namespace orangutan
