#include "providers/provider.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <exec/any_sender_of.hpp>
#include <spdlog/spdlog.h>

#include "providers/anthropic-provider.hpp"
#include "providers/openai-provider.hpp"
#include "utils/format.hpp"
#include "utils/sender-utils.hpp"

namespace orangutan::providers {
    namespace {

        class NonRetryableProviderError final : public std::runtime_error {
        public:
            using std::runtime_error::runtime_error;
        };

        constexpr std::string_view DEFAULT_ENDPOINT_STYLE = "anthropic-messages";

        [[nodiscard]]
        std::string_view resolved_endpoint_style(const ProviderEndpoint &endpoint) noexcept {
            return endpoint.endpoint_style.empty() ? DEFAULT_ENDPOINT_STYLE : std::string_view{endpoint.endpoint_style};
        }

        void ensure_api_key_present(const ProviderEndpoint &endpoint) {
            if (!endpoint.api_key.empty()) {
                return;
            }
            throw MissingApiKeyError("missing API key for endpoint_style '" + std::string{resolved_endpoint_style(endpoint)} + "' model '" + endpoint.model + "'");
        }

        [[nodiscard]]
        std::unique_ptr<Provider> make_anthropic_provider(const ProviderEndpoint &endpoint) {
            return std::make_unique<AnthropicProvider>(endpoint);
        }

        [[nodiscard]]
        std::unique_ptr<Provider> make_openai_provider(const ProviderEndpoint &endpoint) {
            return std::make_unique<OpenAiProvider>(endpoint);
        }

        using ProviderConstructor = std::unique_ptr<Provider> (*)(const ProviderEndpoint &);

        struct ProviderStyleSpec {
            std::string_view style;
            ProviderConstructor constructor;
        };

        constexpr auto PROVIDER_STYLE_SPECS = std::to_array<ProviderStyleSpec>({
            {.style = "anthropic-messages", .constructor = make_anthropic_provider},
            {.style = "openai-chat-completions", .constructor = make_openai_provider},
            {.style = "openai-responses", .constructor = make_openai_provider},
        });

        [[nodiscard]]
        const std::string &supported_provider_styles() {
            static const std::string styles = [] {
                std::string joined;
                bool first_style = true;
                for (const auto &spec : PROVIDER_STYLE_SPECS) {
                    if (!first_style) {
                        joined.append(", ");
                    }
                    joined.append(spec.style);
                    first_style = false;
                }
                return joined;
            }();
            return styles;
        }

        std::unique_ptr<Provider> instantiate_provider(const ProviderEndpoint &endpoint) {
            const auto style = resolved_endpoint_style(endpoint);
            if (const auto *const it = std::ranges::find(PROVIDER_STYLE_SPECS, style, &ProviderStyleSpec::style); it != PROVIDER_STYLE_SPECS.end()) {
                return it->constructor(endpoint);
            }

            throw std::runtime_error("Unknown endpoint_style: " + std::string{style} + ". Supported: " + supported_provider_styles());
        }

        class FallbackProvider final : public Provider {
        public:
            FallbackProvider(std::vector<ProviderEndpoint> endpoints, ProviderFactory factory)
            : endpoints_(std::move(endpoints)),
              factory_(std::move(factory)) {
                if (endpoints_.empty()) {
                    throw std::runtime_error("FallbackProvider requires at least one model endpoint");
                }
                if (factory_ == nullptr) {
                    factory_ = instantiate_provider;
                }
            }

            LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                             int thinking_budget) override {
                return execute_with_fallback([&](Provider &provider) {
                    return provider.chat(system_prompt, messages, tools, max_tokens, thinking_budget);
                });
            }

            LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                    int max_tokens, int thinking_budget) override {
                return execute_with_fallback([&](Provider &provider) {
                    bool emitted_output = false;
                    auto tracking_callback = [&on_event, &emitted_output](const std::string &event_type, const nlohmann::json &data) {
                        emitted_output = true;
                        if (on_event != nullptr) {
                            on_event(event_type, data);
                        }
                    };

                    try {
                        return provider.chat_stream(system_prompt, messages, tools, tracking_callback, max_tokens, thinking_budget);
                    } catch (const std::exception &error) {
                        if (emitted_output) {
                            throw NonRetryableProviderError(error.what());
                        }
                        throw;
                    }
                });
            }

            [[nodiscard]]
            std::string name() const override {
                std::scoped_lock lock(mutex_);
                return std::string{resolved_endpoint_style(endpoints_[preferred_index_])};
            }

            [[nodiscard]]
            std::string current_model() const override {
                std::scoped_lock lock(mutex_);
                return endpoints_[preferred_index_].model;
            }

            [[nodiscard]]
            ProviderUsageStats usage() const override {
                std::scoped_lock lock(mutex_);
                return usage_;
            }

        private:
            template <class... Ts>
            using any_sender_of = exec::any_receiver_ref<stdexec::completion_signatures<Ts...>>::template any_sender<>;

            using response_sender_t = any_sender_of<stdexec::set_value_t(LLMResponse), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

            struct RequestAttemptState {
                std::size_t attempt_index = 0;
                std::vector<std::string> errors;
            };

            struct AttemptResult {
                RequestAttemptState state;
                std::optional<LLMResponse> response;
                std::exception_ptr error;
            };

            std::vector<ProviderEndpoint> endpoints_;
            ProviderFactory factory_;
            mutable std::mutex mutex_;
            std::size_t preferred_index_ = 0;
            std::vector<std::weak_ptr<Provider>> providers_;
            std::shared_ptr<Provider> preferred_provider_;
            ProviderUsageStats usage_;

            std::shared_ptr<Provider> provider_for_index_locked(std::size_t index) {
                if (providers_.empty()) {
                    providers_.resize(endpoints_.size());
                }

                if (index == preferred_index_ && preferred_provider_) {
                    return preferred_provider_;
                }

                if (auto existing = providers_[index].lock()) {
                    if (index == preferred_index_) {
                        preferred_provider_ = existing;
                    }
                    return existing;
                }

                ensure_api_key_present(endpoints_[index]);
                auto created = factory_(endpoints_[index]);
                if (created == nullptr) {
                    throw std::runtime_error("Failed to create provider for model '" + endpoints_[index].model + "'.");
                }

                auto provider = std::shared_ptr<Provider>(std::move(created));
                providers_[index] = provider;
                if (index == preferred_index_) {
                    preferred_provider_ = provider;
                }
                return provider;
            }

            [[nodiscard]]
            static std::runtime_error make_all_failed_error(const std::vector<std::string> &errors) {
                std::string summary = "All configured models failed";
                if (!errors.empty()) {
                    summary.append(" (");
                    summary.append(errors.front());
                    for (std::size_t index = 1; index < errors.size(); ++index) {
                        utils::format_to(summary, "; {}", errors[index]);
                    }
                    summary.push_back(')');
                }
                return std::runtime_error(summary);
            }

            [[nodiscard]]
            std::optional<std::string> advance_after_retryable_failure(RequestAttemptState &state, std::string_view error_message) {
                const auto failed_model = endpoints_[state.attempt_index].model;
                std::optional<std::size_t> next_index;
                std::string next_model;

                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.failed_attempts;

                    std::string message;
                    utils::format_to(message, "{}: {}", failed_model, error_message);
                    state.errors.push_back(std::move(message));

                    if (state.attempt_index + 1 < endpoints_.size()) {
                        const auto candidate_index = state.attempt_index + 1;
                        next_index = candidate_index;
                        next_model = endpoints_[candidate_index].model;
                        if (candidate_index > preferred_index_) {
                            preferred_provider_.reset();
                            preferred_index_ = candidate_index;
                            ++usage_.fallback_switches;
                        }
                    }
                }

                if (!next_index.has_value()) {
                    return std::nullopt;
                }

                spdlog::warn("Model '{}' failed, falling back to '{}': {}", failed_model, next_model, error_message);
                state.attempt_index = *next_index;
                return next_model;
            }

            void record_failed_attempt() {
                std::scoped_lock lock(mutex_);
                ++usage_.failed_attempts;
            }

            template <typename Fn>
            response_sender_t execute_attempt_sender(RequestAttemptState state, Fn &fn) {
                return stdexec::just(std::move(state)) | stdexec::then([this, &fn](RequestAttemptState active_state) {
                           AttemptResult result{.state = std::move(active_state)};

                           try {
                               std::unique_lock lock(mutex_);
                               ++usage_.attempt_count;
                               auto provider = provider_for_index_locked(result.state.attempt_index);
                               lock.unlock();
                               result.response = fn(*provider);
                           } catch (...) {
                               result.error = std::current_exception();
                           }

                           return result;
                       }) |
                       stdexec::let_value([this, &fn](AttemptResult result) -> response_sender_t {
                           if (result.response.has_value()) {
                               return stdexec::just(std::move(*result.response));
                           }

                           try {
                               std::rethrow_exception(result.error);
                           } catch (const NonRetryableProviderError &) {
                               record_failed_attempt();
                               throw;
                           } catch (const MissingApiKeyError &) {
                               record_failed_attempt();
                               throw;
                           } catch (const std::exception &error) {
                               if (!advance_after_retryable_failure(result.state, error.what()).has_value()) {
                                   throw make_all_failed_error(result.state.errors);
                               }
                               return execute_attempt_sender(std::move(result.state), fn);
                           }
                       });
            }

            template <typename Fn>
            LLMResponse execute_with_fallback(Fn fn) {
                RequestAttemptState state;
                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.logical_requests;
                    state.attempt_index = preferred_index_;
                }

                auto pipeline = execute_attempt_sender(std::move(state), fn);
                auto [response] = execution::sync_wait_or_throw(std::move(pipeline), "provider fallback pipeline");
                return response;
            }
        };

        std::vector<ProviderEndpoint> build_provider_chain(const ProviderEndpoint &primary_endpoint, std::vector<ProviderEndpoint> fallback_endpoints) {
            std::vector<ProviderEndpoint> endpoints;
            endpoints.reserve(1 + fallback_endpoints.size());
            endpoints.push_back(primary_endpoint);

            std::unordered_set<std::string> seen_models;
            if (!primary_endpoint.model.empty()) {
                static_cast<void>(seen_models.insert(primary_endpoint.model));
            }

            for (auto &fallback_endpoint : fallback_endpoints) {
                if (fallback_endpoint.model.empty()) {
                    continue;
                }
                if (!seen_models.insert(fallback_endpoint.model).second) {
                    continue;
                }
                endpoints.push_back(std::move(fallback_endpoint));
            }

            return endpoints;
        }

    } // namespace

    std::unique_ptr<Provider> create_provider(const ProviderEndpoint &endpoint, ProviderFactory factory) {
        return create_provider_with_fallbacks(endpoint, {}, std::move(factory));
    }

    std::unique_ptr<Provider> create_provider_with_fallbacks(const ProviderEndpoint &primary_endpoint, std::vector<ProviderEndpoint> fallback_endpoints, ProviderFactory factory) {
        return std::make_unique<FallbackProvider>(build_provider_chain(primary_endpoint, std::move(fallback_endpoints)), std::move(factory));
    }

} // namespace orangutan::providers
