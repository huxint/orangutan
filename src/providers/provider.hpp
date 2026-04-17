#pragma once

#include "types/message.hpp"
#include "types/tool-def.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <exec/any_sender_of.hpp>
#include <nlohmann/json.hpp>
#include <stdexec/execution.hpp>

#include "utils/sender-utils.hpp"
#include "utils/transparent-lookup.hpp"

namespace orangutan::providers {

    enum class provider_kind {
        openai,
        anthropic,
    };

    enum class protocol_kind {
        chat_completions,
        responses,
        messages,
    };

    enum class error_category {
        configuration,
        authentication,
        network,
        rate_limit,
        upstream,
        parsing,
        invalid_request,
        interrupted,
        unknown,
    };

    struct ProviderUsageStats {
        std::size_t logical_requests = 0;
        std::size_t attempt_count = 0;
        std::size_t failed_attempts = 0;
        std::size_t fallback_switches = 0;
    };

    struct ModelTarget {
        std::string profile_name;
        std::string model;
        std::string base_url;
        std::string api_key;
        utils::transparent_string_unordered_map<std::string> headers;
        std::optional<int> default_max_tokens;
        provider_kind provider = provider_kind::anthropic;
        protocol_kind protocol = protocol_kind::messages;
        std::string thinking = "none";
    };

    struct ProviderRoute {
        ModelTarget primary;
        std::vector<ModelTarget> fallbacks;
    };

    struct RequestOptions {
        int max_tokens = 4096;
        int thinking_budget = 0;
        long timeout_seconds = 0;
        bool stream = false;
    };

    struct ProviderRequest {
        std::string system_prompt;
        std::vector<Message> messages;
        std::vector<ToolDef> tools;
        RequestOptions options;
    };

    struct TextDelta {
        std::string text;
    };

    struct ThinkingDelta {
        std::string thinking;
    };

    struct ToolCallStarted {
        std::string id;
        std::string name;
        nlohmann::json input = nlohmann::json::object();
    };

    using ProviderEvent = std::variant<TextDelta, ThinkingDelta, ToolCallStarted>;
    using ProviderEventSink = std::function<void(const ProviderEvent &)>;

    struct ProviderResult {
        LLMResponse response;
        ProviderUsageStats usage_snapshot;
        ModelTarget active_target;
    };

    class ProviderError : public std::runtime_error {
    public:
        ProviderError(error_category category, std::string message, std::optional<ModelTarget> target = {})
        : std::runtime_error(std::move(message)),
          category_(category),
          target_(std::move(target)) {}

        [[nodiscard]]
        error_category category() const noexcept {
            return category_;
        }

        [[nodiscard]]
        bool retryable() const noexcept {
            switch (category_) {
                case error_category::network:
                case error_category::rate_limit:
                case error_category::upstream:
                case error_category::parsing:
                    return true;
                case error_category::configuration:
                case error_category::authentication:
                case error_category::invalid_request:
                case error_category::interrupted:
                case error_category::unknown:
                    return false;
            }

            return false;
        }

        [[nodiscard]]
        const std::optional<ModelTarget> &target() const noexcept {
            return target_;
        }

    private:
        error_category category_;
        std::optional<ModelTarget> target_;
    };

    template <class... Ts>
    using any_sender_of = exec::any_receiver_ref<stdexec::completion_signatures<Ts...>>::template any_sender<>;

    using provider_sender =
        any_sender_of<stdexec::set_value_t(ProviderResult), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    class ProviderBackend {
    public:
        virtual ~ProviderBackend() = default;

        ProviderBackend(const ProviderBackend &) = delete;
        ProviderBackend &operator=(const ProviderBackend &) = delete;
        ProviderBackend(ProviderBackend &&) = delete;
        ProviderBackend &operator=(ProviderBackend &&) = delete;

        [[nodiscard]]
        virtual provider_sender send(ProviderRoute route, ProviderRequest request, ProviderEventSink sink) = 0;

        [[nodiscard]]
        virtual ProviderUsageStats usage() const = 0;

        [[nodiscard]]
        virtual std::optional<ModelTarget> active_target() const = 0;

        [[nodiscard]]
        virtual std::string label() const = 0;

    protected:
        ProviderBackend() = default;
    };

    class ProviderSystem;

    class RequestBuilder {
    public:
        RequestBuilder(std::shared_ptr<ProviderBackend> backend, ProviderRoute route);

        auto route(this auto &&self, ProviderRoute route) -> decltype(auto) {
            self.route_ = std::move(route);
            return std::forward<decltype(self)>(self);
        }

        auto system(this auto &&self, std::string_view prompt) -> decltype(auto) {
            self.request_.system_prompt = std::string(prompt);
            return std::forward<decltype(self)>(self);
        }

        auto messages(this auto &&self, std::span<const Message> messages) -> decltype(auto) {
            self.request_.messages.assign(messages.begin(), messages.end());
            return std::forward<decltype(self)>(self);
        }

        auto append_message(this auto &&self, Message message) -> decltype(auto) {
            self.request_.messages.push_back(std::move(message));
            return std::forward<decltype(self)>(self);
        }

        auto tools(this auto &&self, std::span<const ToolDef> tools) -> decltype(auto) {
            self.request_.tools.assign(tools.begin(), tools.end());
            return std::forward<decltype(self)>(self);
        }

        auto max_tokens(this auto &&self, int max_tokens) -> decltype(auto) {
            self.request_.options.max_tokens = max_tokens;
            return std::forward<decltype(self)>(self);
        }

        auto thinking_budget(this auto &&self, int thinking_budget) -> decltype(auto) {
            self.request_.options.thinking_budget = thinking_budget;
            return std::forward<decltype(self)>(self);
        }

        auto timeout(this auto &&self, long timeout_seconds) -> decltype(auto) {
            self.request_.options.timeout_seconds = timeout_seconds;
            return std::forward<decltype(self)>(self);
        }

        auto stream(this auto &&self, bool enabled = true) -> decltype(auto) {
            self.request_.options.stream = enabled;
            return std::forward<decltype(self)>(self);
        }

        auto on_event(this auto &&self, ProviderEventSink sink) -> decltype(auto) {
            self.sink_ = std::move(sink);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        provider_sender send() const;

        [[nodiscard]]
        std::expected<ProviderResult, ProviderError> try_send_blocking() const;

        [[nodiscard]]
        ProviderResult send_blocking() const {
            auto result = try_send_blocking();
            if (!result) {
                throw std::move(result).error();
            }
            return std::move(*result);
        }

    private:
        std::shared_ptr<ProviderBackend> backend_;
        ProviderRoute route_;
        ProviderRequest request_;
        ProviderEventSink sink_;
    };

    class ProviderSystem {
    public:
        ProviderSystem();
        explicit ProviderSystem(std::shared_ptr<ProviderBackend> backend);

        [[nodiscard]]
        RequestBuilder route(ProviderRoute route) const;

        [[nodiscard]]
        ProviderUsageStats usage() const;

        [[nodiscard]]
        std::optional<ModelTarget> active_target() const;

        [[nodiscard]]
        std::string current_model() const;

        [[nodiscard]]
        std::string label() const;

    private:
        std::shared_ptr<ProviderBackend> backend_;
    };

    [[nodiscard]]
    provider_kind parse_provider_kind(std::string_view token);

    [[nodiscard]]
    protocol_kind parse_protocol_kind(std::string_view token);

    [[nodiscard]]
    std::string target_label(const ModelTarget &target);

    [[nodiscard]]
    response_stop_reason map_stop_reason(std::string_view token);

} // namespace orangutan::providers

namespace orangutan {

    using providers::error_category;
    using providers::map_stop_reason;
    using providers::ModelTarget;
    using providers::parse_protocol_kind;
    using providers::parse_provider_kind;
    using providers::protocol_kind;
    using providers::ProviderBackend;
    using providers::ProviderError;
    using providers::ProviderEvent;
    using providers::ProviderEventSink;
    using providers::ProviderResult;
    using providers::ProviderRoute;
    using providers::ProviderSystem;
    using providers::ProviderUsageStats;
    using providers::provider_kind;
    using providers::RequestBuilder;
    using providers::RequestOptions;
    using providers::ProviderRequest;
    using providers::target_label;
    using providers::TextDelta;
    using providers::ThinkingDelta;
    using providers::ToolCallStarted;

} // namespace orangutan
