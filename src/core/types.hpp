#pragma once

#include <spdlog/common.h>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <string_view>
#include <utility>
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

        enum class role : base::u8 {
            user,
            assistant,
        };

        enum class origin : base::u8 {
            cli,
            channel,
            web,
        };
    }

    struct Text {
        std::string text;

        Text() = default;

        template <typename Str>
            requires std::convertible_to<Str, std::string_view>
        Text(Str value)
        : text(value) {}
    };

    struct Thinking {
        std::string thinking;

        Thinking() = default;

        template <typename Str>
            requires std::convertible_to<Str, std::string_view>
        Thinking(Str value)
        : thinking(value) {}
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

    class Message {
    public:
        explicit Message(base::role role)
        : role_{role} {}

        Message(base::role role, std::initializer_list<Content> blocks)
        : role_{role} {
            for (const auto &block : blocks) {
                content_.push_back(block);
            }
        }

        Message(base::role role, std::vector<Content> blocks)
        : role_{role},
          content_{std::move(blocks)} {}

        [[nodiscard]]
        base::role role() const noexcept {
            return role_;
        }

        auto text(this auto &&self, Text value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto thinking(this auto &&self, Thinking value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto tool_use(this auto &&self, ToolUse value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto tool_result(this auto &&self, ToolResult value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        static Message user() {
            return Message(base::role::user);
        }

        static Message assistant() {
            return Message(base::role::assistant);
        }

        [[nodiscard]]
        auto begin() noexcept {
            return content_.begin();
        }

        [[nodiscard]]
        auto end() noexcept {
            return content_.end();
        }

        [[nodiscard]]
        auto begin() const noexcept {
            return content_.begin();
        }

        [[nodiscard]]
        auto end() const noexcept {
            return content_.end();
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return content_.empty();
        }

        [[nodiscard]]
        std::size_t size() const noexcept {
            return content_.size();
        }

    private:
        base::role role_;
        std::vector<Content> content_;

        static auto append(auto &&self, auto &&value) -> decltype(self) {
            self.content_.emplace_back(std::forward<decltype(value)>(value));
            return std::forward<decltype(self)>(self);
        }
    };

    class Conversation {
    public:
        [[nodiscard]]
        std::size_t size() const noexcept {
            return messages_.size();
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return messages_.empty();
        }

        auto append(Message msg) -> Conversation & {
            messages_.push_back(std::move(msg));
            return *this;
        }

        // conversation.user("hello", Thinking{"..."})
        template <typename... Args>
        auto user(Args &&...args) -> Conversation & {
            return emplace(base::role::user, std::forward<Args>(args)...);
        }

        template <typename... Args>
        auto assistant(Args &&...args) -> Conversation & {
            return emplace(base::role::assistant, std::forward<Args>(args)...);
        }

        [[nodiscard]]
        auto begin() noexcept {
            return messages_.begin();
        }

        [[nodiscard]]
        auto end() noexcept {
            return messages_.end();
        }

        [[nodiscard]]
        auto begin() const noexcept {
            return messages_.begin();
        }

        [[nodiscard]]
        auto end() const noexcept {
            return messages_.end();
        }

    private:
        std::vector<Message> messages_;

        template <typename... Args>
        auto emplace(base::role role, Args &&...args) -> Conversation & {
            auto msg = Message(role);
            (emplace_one(msg, std::forward<Args>(args)), ...);
            messages_.push_back(std::move(msg));
            return *this;
        }

        template <typename T>
        static void emplace_one(Message &msg, T &&value) {
            using U = std::remove_cvref_t<T>;

            if constexpr (std::convertible_to<U, Text>) {
                msg.text(std::forward<T>(value));
            } else if constexpr (std::same_as<U, Thinking>) {
                msg.thinking(std::forward<T>(value));
            } else if constexpr (std::same_as<U, ToolUse>) {
                msg.tool_use(std::forward<T>(value));
            } else if constexpr (std::same_as<U, ToolResult>) {
                msg.tool_result(std::forward<T>(value));
            }
        }
    };

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
                    nlohmann::json json = {{"type", "tool_result"}, {"tool_use_id", blk.tool_use_id}, {"content", blk.content}};
                    if (blk.is_error) {
                        json["is_error"] = true;
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

    // Tool definition sent to the API
    struct ToolDef {
        std::string name;
        std::string description;
        nlohmann::json input_schema; // JSON Schema
    };

    // API response metadata
    struct LLMResponse {
        std::string stop_reason; // "end_turn", "tool_use", etc.
        std::vector<Content> content;
    };
}
