#include "providers/provider.hpp"

#include <memory>

#include <fmt/format.h>

#include "providers/execution/runtime-backend.hpp"
#include "utils/enum-string.hpp"
#include "utils/string.hpp"

namespace orangutan::providers {

    namespace {

        [[nodiscard]]
        std::string format_parse_error(std::string_view kind, std::string_view token) {
            return fmt::format("unsupported {}: {}", kind, token);
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
            auto [result] = utils::sync_wait_or_throw(send(), "provider sender");
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
        return {backend_, std::move(route)};
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

    provider_kind parse_provider_kind(std::string_view token) {
        if (const auto parsed = utils::parse_enum<provider_kind>(token); parsed.has_value()) {
            return *parsed;
        }
        throw ProviderError(error_category::configuration, format_parse_error("provider", token));
    }

    protocol_kind parse_protocol_kind(std::string_view token) {
        if (const auto parsed = utils::parse_enum<protocol_kind>(token); parsed.has_value()) {
            return *parsed;
        }
        throw ProviderError(error_category::configuration, format_parse_error("protocol", token));
    }

    std::string target_label(const ModelTarget &target) {
        return std::string(utils::enum_name(target.provider)) + ":" + std::string(utils::enum_name_kebab(target.protocol)) + ":" + target.model;
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
