#pragma once

#include "types/content.hpp"
#include "types/message.hpp"

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

namespace orangutan {

    // Serialize content to JSON for API request
    inline nlohmann::json content_block_to_json(const Content &block) {
        return std::visit(
            [](auto &&blk) -> nlohmann::json {
                using T = std::decay_t<decltype(blk)>;
                if constexpr (std::same_as<T, Text>) {
                    return {{"type", "text"}, {"text", blk.text}};
                } else if constexpr (std::same_as<T, Thinking>) {
                    return {{"type", "thinking"}, {"thinking", blk.thinking}};
                } else if constexpr (std::same_as<T, ToolUse>) {
                    return {{"type", "tool_use"}, {"id", blk.id}, {"name", blk.name}, {"input", blk.input}};
                } else if constexpr (std::same_as<T, ToolResult>) {
                    nlohmann::json json = {{"type", "tool_result"}, {"tool_use_id", blk.tool_use_id}};
                    if (blk.is_error) {
                        json["is_error"] = true;
                    }
                    if (blk.images.empty()) {
                        json["content"] = blk.content;
                    } else {
                        nlohmann::json content_array = nlohmann::json::array();
                        if (!blk.content.empty()) {
                            content_array.push_back({{"type", "text"}, {"text", blk.content}});
                        }
                        for (const auto &img : blk.images) {
                            content_array.push_back({
                                {"type", "image"},
                                {"source", {{"type", "base64"}, {"media_type", img.media_type}, {"data", img.data}}},
                            });
                        }
                        json["content"] = content_array;
                    }
                    return json;
                }
            },
            block);
    }

    // Serialize a Message to JSON
    inline nlohmann::json message_to_json(const Message &msg) {
        nlohmann::json json;
        json["role"] = magic_enum::enum_name(msg.role());
        json["content"] = nlohmann::json::array();
        for (const auto &block : msg) {
            json["content"].push_back(content_block_to_json(block));
        }
        return json;
    }
} // namespace orangutan
