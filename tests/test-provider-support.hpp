#pragma once

#include "providers/provider.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

namespace orangutan::testing {

    inline providers::ModelTarget make_test_target(std::string model = "test-model", providers::provider_kind provider = providers::provider_kind::openai,
                                                   providers::protocol_kind protocol = providers::protocol_kind::chat_completions, std::string api_key = "test-key",
                                                   std::string base_url = "https://example.test", std::string profile_name = "test-profile") {
        return providers::ModelTarget{
            .profile_name = std::move(profile_name),
            .model = std::move(model),
            .base_url = std::move(base_url),
            .api_key = std::move(api_key),
            .provider = provider,
            .protocol = protocol,
        };
    }

    inline providers::ProviderRoute make_test_route(std::string model = "test-model", providers::provider_kind provider = providers::provider_kind::openai,
                                                    providers::protocol_kind protocol = providers::protocol_kind::chat_completions, std::string api_key = "test-key",
                                                    std::string base_url = "https://example.test", std::string profile_name = "test-profile") {
        return providers::ProviderRoute{
            .primary = make_test_target(std::move(model), provider, protocol, std::move(api_key), std::move(base_url), std::move(profile_name)),
        };
    }

    inline LLMResponse make_text_response(std::string text, response_stop_reason stop_reason = response_stop_reason::end_turn) {
        return LLMResponse{
            .stop_reason = stop_reason,
            .content = {Text{std::move(text)}},
        };
    }

    class FakeProviderBackend final : public providers::ProviderBackend {
    public:
        struct Invocation {
            providers::ProviderRoute route;
            providers::ProviderRequest request;
            bool has_sink = false;
        };

        using handler_type = std::function<providers::ProviderResult(const providers::ProviderRoute &, const providers::ProviderRequest &, const providers::ProviderEventSink &)>;

        explicit FakeProviderBackend(handler_type handler = {})
        : handler_(std::move(handler)) {}

        [[nodiscard]]
        providers::provider_sender send(providers::ProviderRoute route, providers::ProviderRequest request, providers::ProviderEventSink sink) override {
            return stdexec::just(std::move(route), std::move(request), std::move(sink)) |
                   stdexec::then([this](providers::ProviderRoute active_route, providers::ProviderRequest active_request, providers::ProviderEventSink active_sink) {
                       invocations_.push_back(Invocation{
                           .route = active_route,
                           .request = active_request,
                           .has_sink = active_sink != nullptr,
                       });

                       if (handler_ != nullptr) {
                           auto result = handler_(active_route, active_request, active_sink);
                           active_target_ = result.active_target;
                           return result;
                       }

                       if (!active_target_.has_value()) {
                           active_target_ = active_route.primary;
                       }

                       return providers::ProviderResult{
                           .response = {},
                           .usage_snapshot = usage_,
                           .active_target = *active_target_,
                       };
                   });
        }

        [[nodiscard]]
        providers::ProviderUsageStats usage() const override {
            return usage_;
        }

        [[nodiscard]]
        std::optional<providers::ModelTarget> active_target() const override {
            return active_target_;
        }

        [[nodiscard]]
        std::string label() const override {
            return label_;
        }

        void set_handler(handler_type handler) {
            handler_ = std::move(handler);
        }

        void set_usage(providers::ProviderUsageStats usage) {
            usage_ = usage;
        }

        void set_active_target(providers::ModelTarget target) {
            active_target_ = std::move(target);
        }

        void set_label(std::string label) {
            label_ = std::move(label);
        }

        [[nodiscard]]
        const std::vector<Invocation> &invocations() const {
            return invocations_;
        }

    private:
        handler_type handler_;
        providers::ProviderUsageStats usage_;
        std::optional<providers::ModelTarget> active_target_;
        std::vector<Invocation> invocations_;
        std::string label_ = "test-provider";
    };

    inline std::shared_ptr<FakeProviderBackend> make_fake_provider_backend(FakeProviderBackend::handler_type handler = {}) {
        return std::make_shared<FakeProviderBackend>(std::move(handler));
    }

    inline providers::ProviderSystem make_provider_system(const std::shared_ptr<FakeProviderBackend> &backend) {
        return providers::ProviderSystem(backend);
    }

} // namespace orangutan::testing
