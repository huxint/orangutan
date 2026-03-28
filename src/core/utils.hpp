#pragma once

#include "types.hpp"

namespace orangutan::core {
    // A message in the conversation
    class Message {
    public:
        explicit Message(base::role role)
        : role_{role} {}

        [[nodiscard]]
        base::role role() const noexcept {
            return role_;
        }

        auto text(this auto &&self, block::text value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto thinking(this auto &&self, block::thinking value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto tool_use(this auto &&self, block::tool_use value) -> decltype(auto) {
            return append(std::forward<decltype(self)>(self), std::move(value));
        }

        auto tool_result(this auto &&self, block::tool_result value) -> decltype(auto) {
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

    private:
        base::role role_;
        std::vector<block::content> content_;

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

        // conversation.user(block::text{"hello"}, block::thinking{"..."})
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

            if constexpr (std::same_as<U, block::text>) {
                msg.text(std::forward<T>(value));
            } else if constexpr (std::same_as<U, block::thinking>) {
                msg.thinking(std::forward<T>(value));
            } else if constexpr (std::same_as<U, block::tool_use>) {
                msg.tool_use(std::forward<T>(value));
            } else if constexpr (std::same_as<U, block::tool_result>) {
                msg.tool_result(std::forward<T>(value));
            }
        }
    };

    // Serialize content to JSON for API request
    deps::json content_block_to_json(const core::block::content &block) {
        return std::visit(
            [](auto &&blk) -> deps::json {
                using T = std::decay_t<decltype(blk)>;
                if constexpr (std::same_as<T, core::block::text>) {
                    return {{"type", "text"}, {"text", blk.text}};
                } else if constexpr (std::same_as<T, core::block::thinking>) {
                    return {{"type", "thinking"}, {"thinking", blk.thinking}};
                } else if constexpr (std::same_as<T, core::block::tool_use>) {
                    return {{"type", "tool_use"}, {"id", blk.id}, {"name", blk.name}, {"input", blk.input}};
                } else if constexpr (std::same_as<T, core::block::tool_result>) {
                    deps::json json = {{"type", "tool_result"}, {"tool_use_id", blk.tool_use_id}, {"content", blk.content}};
                    if (blk.is_error) {
                        json["is_error"] = true;
                    }
                    return json;
                }
            },
            block);
    }

    // Serialize a Message to JSON
    deps::json message_to_json(const core::Message &msg) {
        deps::json json;
        json["role"] = magic_enum::enum_name(msg.role());
        json["content"] = deps::json::array();
        for (const auto &block : msg) {
            json["content"].push_back(content_block_to_json(block));
        }
        return json;
    }
} // namespace orangutan::core
