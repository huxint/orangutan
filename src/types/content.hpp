#pragma once

#include <concepts>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace orangutan {

    struct Text {
        std::string text;

        Text() = default;

        template <typename T>
            requires std::constructible_from<std::string, T &&>
        explicit Text(T &&value)
        : text(std::forward<T>(value)) {}

        explicit Text(const char *value)
        : text(value != nullptr ? value : "") {}
    };

    struct Thinking {
        std::string thinking;

        Thinking() = default;

        template <typename T>
            requires std::constructible_from<std::string, T &&>
        explicit Thinking(T &&value)
        : thinking(std::forward<T>(value)) {}

        explicit Thinking(const char *value)
        : thinking(value != nullptr ? value : "") {}
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
        struct ImageBlock {
            std::string media_type;
            std::string data;
        };

        std::string tool_use_id;
        std::string content;
        bool is_error = false;
        std::vector<ImageBlock> images;

        ToolResult() = default;
        ToolResult(std::string tool_use_id, std::string content, bool is_error = false)
        : tool_use_id(std::move(tool_use_id)),
          content(std::move(content)),
          is_error(is_error) {}
    };

    using Content = std::variant<Text, Thinking, ToolUse, ToolResult>;
} // namespace orangutan
