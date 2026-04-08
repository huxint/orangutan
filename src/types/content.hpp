#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace orangutan {

    struct Text {
        std::string text;

        Text() = default;
        Text(std::string value)
        : text(std::move(value)) {}
        Text(std::string_view value)
        : text(value) {}
        Text(const char *value)
        : text(value != nullptr ? value : "") {}
    };

    struct Thinking {
        std::string thinking;

        Thinking() = default;
        Thinking(std::string value)
        : thinking(std::move(value)) {}
        Thinking(std::string_view value)
        : thinking(value) {}
        Thinking(const char *value)
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
}
