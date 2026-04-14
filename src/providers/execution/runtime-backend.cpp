#include "providers/execution/runtime-backend.hpp"

#include <mutex>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "providers/protocols/provider-registry.hpp"
#include "providers/transport/http-transport.hpp"

namespace orangutan::providers::execution {
    namespace {

        using protocols::ProviderRegistry;
        using transport::HttpTransport;

        [[nodiscard]]
        std::string target_key(const ModelTarget &target) {
            return target.profile_name + "|" + std::string(to_string(target.provider)) + "|" + std::string(to_string(target.protocol)) + "|" + target.model + "|" + target.base_url;
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
                    return std::string(to_string(active_target_->provider));
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
            ProviderResult execute(ProviderRoute route, ProviderRequest request, ProviderEventSink sink) {
                auto targets = flatten_route(route);
                if (targets.empty()) {
                    throw ProviderError(error_category::configuration, "provider route does not contain any models");
                }

                std::size_t begin_index = 0;
                {
                    std::scoped_lock lock(mutex_);
                    ++usage_.logical_requests;
                    begin_index = starting_index(targets, preferred_target_key_);
                }

                std::vector<std::string> failures;
                ProviderError last_error(error_category::unknown, "provider execution failed");

                for (std::size_t index = begin_index; index < targets.size(); ++index) {
                    auto target = targets[index];
                    bool emitted_stream_event = false;
                    auto handle_failure = [&](const ProviderError &error) {
                        {
                            std::scoped_lock lock(mutex_);
                            ++usage_.failed_attempts;
                        }

                        failures.push_back(target_label(target) + ": " + error.what());
                        last_error = error;

                        if (!error.retryable() || emitted_stream_event || index + 1 >= targets.size()) {
                            return false;
                        }

                        {
                            std::scoped_lock lock(mutex_);
                            ++usage_.fallback_switches;
                        }

                        spdlog::warn("provider target '{}' failed, falling back: {}", target_label(target), error.what());
                        return true;
                    };

                    try {
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
                        assembly.auth->apply(target, http_request.headers);

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
                            .active_target = std::move(target),
                        };
                    } catch (const ProviderError &error) {
                        if (!handle_failure(error)) {
                            break;
                        }
                    } catch (const nlohmann::json::exception &error) {
                        if (!handle_failure(ProviderError(error_category::parsing, error.what(), target))) {
                            break;
                        }
                    } catch (const std::exception &error) {
                        if (!handle_failure(ProviderError(error_category::unknown, error.what(), target))) {
                            break;
                        }
                    }
                }

                std::string message = "all configured models failed";
                if (!failures.empty()) {
                    message += " (";
                    for (std::size_t index = 0; index < failures.size(); ++index) {
                        if (index > 0) {
                            message += "; ";
                        }
                        message += failures[index];
                    }
                    message.push_back(')');
                }

                throw ProviderError(last_error.category(), std::move(message), last_error.target());
            }
        };

    } // namespace

    std::shared_ptr<ProviderBackend> make_runtime_backend() {
        return std::make_shared<RuntimeBackend>();
    }

} // namespace orangutan::providers::execution
