#pragma once

#include "types/base.hpp"
#include "types/content.hpp"

#include <concepts>
#include <initializer_list>
#include <vector>

namespace orangutan {

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
} // namespace orangutan
