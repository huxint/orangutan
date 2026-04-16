#include "providers/provider.hpp"

#include <memory>

#include <magic_enum/magic_enum.hpp>

#include "providers/execution/runtime-backend.hpp"
#include "utils/string.hpp"

namespace orangutan::providers {

    namespace {

        [[nodiscard]]
        std::string format_parse_error(std::string_view kind, std::string_view token) {
            return std::string("unsupported ") + std::string(kind) + ": " + std::string(token);
        }

    } // namespace

    RequestBuilder::RequestBuilder(std::shared_ptr<ProviderBackend> backend, ProviderRoute route)
    : backend_(std::move(backend)),
      route_(std::move(route)) {}

    provider_sender RequestBuilder::send() const {
        return backend_->send(route_, request_, sink_);
    }

    std::expected<ProviderResult, ProviderError> RequestBuilder::try_send_blocking() const {
        try {
            auto [result] = execution::sync_wait_or_throw(send(), "provider sender");
            return result;
        } catch (const ProviderError &error) {
            return std::unexpected(error);
        } catch (const std::exception &error) {
            return std::unexpected(ProviderError(error_category::unknown, error.what()));
        }
    }

    ProviderSystem::ProviderSystem()
    : backend_(execution::make_runtime_backend()) {}

    ProviderSystem::ProviderSystem(std::shared_ptr<ProviderBackend> backend)
    : backend_(std::move(backend)) {}

    RequestBuilder ProviderSystem::route(ProviderRoute route) const {
        return RequestBuilder(backend_, std::move(route));
    }

    ProviderUsageStats ProviderSystem::usage() const {
        return backend_->usage();
    }

    std::optional<ModelTarget> ProviderSystem::active_target() const {
        return backend_->active_target();
    }

    std::string ProviderSystem::current_model() const {
        const auto target = active_target();
        return target.has_value() ? target->model : std::string{};
    }

    std::string ProviderSystem::label() const {
        return backend_->label();
    }

    std::string_view to_string(provider_kind provider) noexcept {
        switch (provider) {
            case provider_kind::openai:
                return "openai";
            case provider_kind::anthropic:
                return "anthropic";
        }

        return "unknown";
    }

    std::string_view to_string(protocol_kind protocol) noexcept {
        switch (protocol) {
            case protocol_kind::chat_completions:
                return "chat-completions";
            case protocol_kind::responses:
                return "responses";
            case protocol_kind::messages:
                return "messages";
        }

        return "unknown";
    }

    std::string_view to_string(error_category category) noexcept {
        switch (category) {
            case error_category::configuration:
                return "configuration";
            case error_category::authentication:
                return "authentication";
            case error_category::network:
                return "network";
            case error_category::rate_limit:
                return "rate_limit";
            case error_category::upstream:
                return "upstream";
            case error_category::parsing:
                return "parsing";
            case error_category::invalid_request:
                return "invalid_request";
            case error_category::interrupted:
                return "interrupted";
            case error_category::unknown:
                return "unknown";
        }

        return "unknown";
    }

    provider_kind parse_provider_kind(std::string_view token) {
        const auto normalized = normalize_enum_token(token);
        if (normalized == "openai") {
            return provider_kind::openai;
        }
        if (normalized == "anthropic") {
            return provider_kind::anthropic;
        }
        throw ProviderError(error_category::configuration, format_parse_error("provider", token));
    }

    protocol_kind parse_protocol_kind(std::string_view token) {
        const auto normalized = normalize_enum_token(token);
        if (normalized == "chat_completions") {
            return protocol_kind::chat_completions;
        }
        if (normalized == "responses") {
            return protocol_kind::responses;
        }
        if (normalized == "messages") {
            return protocol_kind::messages;
        }
        throw ProviderError(error_category::configuration, format_parse_error("protocol", token));
    }

    std::string target_label(const ModelTarget &target) {
        return std::string(to_string(target.provider)) + ":" + std::string(to_string(target.protocol)) + ":" + target.model;
    }

    response_stop_reason map_stop_reason(std::string_view token) {
        const auto normalized = normalize_enum_token(token);
        if (normalized == "end_turn" || normalized == "stop" || normalized == "completed") {
            return response_stop_reason::end_turn;
        }
        if (normalized == "tool_use" || normalized == "tool_calls" || normalized == "function_call") {
            return response_stop_reason::tool_use;
        }
        if (normalized == "max_tokens" || normalized == "length" || normalized == "max_output_tokens") {
            return response_stop_reason::max_tokens;
        }
        return response_stop_reason::unknown;
    }

} // namespace orangutan::providers
