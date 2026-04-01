#pragma once

#include <concepts>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace orangutan {

    struct Text {
        std::string text;

        Text() = default;

        template <typename Str>
            requires std::convertible_to<Str, std::string_view>
        Text(Str value)
        : text(static_cast<std::string>(std::string_view{value})) {}
    };

    struct Thinking {
        std::string thinking;

        Thinking() = default;

        template <typename Str>
            requires std::convertible_to<Str, std::string_view>
        Thinking(Str value)
        : thinking(static_cast<std::string>(std::string_view{value})) {}
    };

    struct ToolUse {
        std::string id;
        std::string name;
        nlohmann::json input;

        ToolUse() = default;
        ToolUse(std::string id, std::string name, nlohmann::json input)
        : id(std::move(id)),
          name(std::move(name)),
          input(std::move(input)) {}
    };

    struct ToolResult {
        std::string tool_use_id;
        std::string content;
        bool is_error = false;

        ToolResult() = default;
        ToolResult(std::string tool_use_id, std::string content, bool is_error = false)
        : tool_use_id(std::move(tool_use_id)),
          content(std::move(content)),
          is_error(is_error) {}
    };

    using Content = std::variant<Text, Thinking, ToolUse, ToolResult>;
}
