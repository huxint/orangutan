#include "providers/execution/runtime-backend.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

#include <exec/repeat_effect_until.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "providers/protocols/provider-registry.hpp"
#include "providers/transport/http-transport.hpp"
#include "utils/enum-string.hpp"
#include "utils/sender-utils.hpp"

namespace orangutan::providers::execution {
    namespace {

        using protocols::ProviderRegistry;
        using transport::HttpTransport;

        [[nodiscard]]
        std::string target_key(const ModelTarget &target) {
            return target.profile_name + "|" + std::string(utils::enum_name(target.provider)) + "|" + std::string(utils::enum_name_kebab(target.protocol)) + "|" +
                   target.model + "|" + target.base_url;
        }

        [[nodiscard]]
        std::vector<ModelTarget> flatten_route(const ProviderRoute &route) {
            std::vector<ModelTarget> targets;
            targets.reserve(1 + route.fallbacks.size());

            std::unordered_set<std::string> seen;
            const auto append = [&](const ModelTarget &target) {
                if (target.model.empty()) {
                    return;
                }
                if (!seen.insert(target_key(target)).second) {
                    return;
                }
                targets.push_back(target);
            };

            append(route.primary);
            for (const auto &fallback : route.fallbacks) {
                append(fallback);
            }

            return targets;
        }

        class RuntimeBackend final : public ProviderBackend {
        public:
            [[nodiscard]]
            provider_sender send(ProviderRoute route, ProviderRequest request, ProviderEventSink sink) override {
                return stdexec::just(std::move(route), std::move(request), std::move(sink)) |
                       stdexec::then([this](ProviderRoute active_route, ProviderRequest active_request, ProviderEventSink active_sink) {
                           return execute(std::move(active_route), std::move(active_request), std::move(active_sink));
                       });
            }

            [[nodiscard]]
            ProviderUsageStats usage() const override {
                std::scoped_lock lock(mutex_);
                return usage_;
            }

            [[nodiscard]]
            std::optional<ModelTarget> active_target() const override {
                std::scoped_lock lock(mutex_);
                return active_target_;
            }

            [[nodiscard]]
            std::string label() const override {
                std::scoped_lock lock(mutex_);
                if (active_target_.has_value()) {
                    return std::string(utils::enum_name(active_target_->provider));
                }
                return "providers";
            }

        private:
            ProviderRegistry registry_;
            HttpTransport transport_;
            mutable std::mutex mutex_;
            ProviderUsageStats usage_;
            std::optional<ModelTarget> active_target_;
            std::string preferred_target_key_;

            struct AttemptState {
                std::vector<ModelTarget> targets;
                ProviderRequest request;
                ProviderEventSink sink;
                std::size_t index = 0;
                std::vector<std::string> failures;
                ProviderError last_error{error_category::unknown, "provider execution failed"};
                std::optional<ProviderResult> result;
            };

            [[nodiscard]]
            static std::size_t starting_index(const std::vector<ModelTarget> &targets, std::string_view preferred_target_key) {
                if (preferred_target_key.empty()) {
                    return 0;
                }

                for (std::size_t index = 0; index < targets.size(); ++index) {
                    if (target_key(targets[index]) == preferred_target_key) {
                        return index;
                    }
                }

                return 0;
            }

            [[nodiscard]]
            ProviderResult attempt_target(const ModelTarget &target, const ProviderRequest &request, const ProviderEventSink &sink, bool &emitted_stream_event) {
                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.attempt_count;
                }

                if (target.api_key.empty()) {
                    throw ProviderError(error_category::configuration, "missing api key for " + target_label(target), target);
                }

                auto assembly = registry_.resolve(target.provider, target.protocol);
                auto effective_request = request;
                if (target.default_max_tokens.has_value()) {
                    effective_request.options.max_tokens = *target.default_max_tokens;
                }
                if (effective_request.options.timeout_seconds <= 0) {
                    effective_request.options.timeout_seconds = effective_request.options.stream ? 300 : 120;
                }

                auto http_request = assembly.adapter->build_request(target, effective_request);
                http_request.timeout_seconds = effective_request.options.timeout_seconds;
                assembly.auth(target, http_request.headers);

                auto tracking_sink = [&](const ProviderEvent &event) {
                    emitted_stream_event = true;
                    if (sink != nullptr) {
                        sink(event);
                    }
                };

                LLMResponse response;
                if (effective_request.options.stream) {
                    auto decoder = assembly.adapter->make_stream_decoder(tracking_sink);
                    transport_.stream_sse(http_request, target, [&decoder](std::string_view event_name, std::string_view payload) {
                        decoder->on_event(event_name, payload);
                    });
                    response = decoder->finish();
                } else {
                    const auto http_response = transport_.post(http_request, target);
                    response = assembly.adapter->parse_response(http_response);
                }

                ProviderUsageStats snapshot;
                {
                    std::scoped_lock lock(mutex_);
                    active_target_ = target;
                    preferred_target_key_ = target_key(target);
                    snapshot = usage_;
                }

                return ProviderResult{
                    .response = std::move(response),
                    .usage_snapshot = snapshot,
                    .active_target = target,
                };
            }

            /// Returns true if the retry loop should stop after this failure.
            [[nodiscard]]
            bool record_failure(AttemptState &state, const ModelTarget &target, const ProviderError &error, bool emitted_stream_event) {
                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.failed_attempts;
                }

                state.failures.push_back(target_label(target) + ": " + error.what());
                state.last_error = error;

                const bool is_last_target = state.index + 1 >= state.targets.size();
                if (!error.retryable() || emitted_stream_event || is_last_target) {
                    return true;
                }

                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.fallback_switches;
                }

                spdlog::warn("provider target '{}' failed, falling back: {}", target_label(target), error.what());
                return false;
            }

            [[nodiscard]]
            static ProviderError compose_final_error(const AttemptState &state) {
                std::string message = "all configured models failed";
                if (!state.failures.empty()) {
                    message += " (";
                    for (std::size_t index = 0; index < state.failures.size(); ++index) {
                        if (index > 0) {
                            message += "; ";
                        }
                        message += state.failures[index];
                    }
                    message.push_back(')');
                }
                return ProviderError(state.last_error.category(), std::move(message), state.last_error.target());
            }

            [[nodiscard]]
            ProviderResult execute(ProviderRoute route, ProviderRequest request, ProviderEventSink sink) {
                auto state = std::make_shared<AttemptState>();
                state->targets = flatten_route(route);
                if (state->targets.empty()) {
                    throw ProviderError(error_category::configuration, "provider route does not contain any models");
                }
                state->request = std::move(request);
                state->sink = std::move(sink);

                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.logical_requests;
                    state->index = starting_index(state->targets, preferred_target_key_);
                }

                auto attempt_once = stdexec::just()
                                  | stdexec::then([this, state]() -> bool {
                                        const auto &target = state->targets[state->index];
                                        bool emitted_stream_event = false;
                                        try {
                                            state->result.emplace(attempt_target(target, state->request, state->sink, emitted_stream_event));
                                            return true;
                                        } catch (const ProviderError &e) {
                                            if (record_failure(*state, target, e, emitted_stream_event)) {
                                                return true;
                                            }
                                        } catch (const nlohmann::json::exception &e) {
                                            if (record_failure(*state, target, ProviderError(error_category::parsing, e.what(), target), emitted_stream_event)) {
                                                return true;
                                            }
                                        } catch (const std::exception &e) {
                                            if (record_failure(*state, target, ProviderError(error_category::unknown, e.what(), target), emitted_stream_event)) {
                                                return true;
                                            }
                                        }
                                        ++state->index;
                                        return false;
                                    });

                auto pipeline = std::move(attempt_once)
                              | exec::repeat_effect_until()
                              | stdexec::then([state] {
                                    if (state->result.has_value()) {
                                        return std::move(*state->result);
                                    }
                                    throw compose_final_error(*state);
                                });

                auto [result] = utils::sync_wait_or_throw(std::move(pipeline), "provider runtime retry pipeline");
                return result;
            }
        };

    } // namespace

    std::shared_ptr<ProviderBackend> make_runtime_backend() {
        return std::make_shared<RuntimeBackend>();
    }

} // namespace orangutan::providers::execution
