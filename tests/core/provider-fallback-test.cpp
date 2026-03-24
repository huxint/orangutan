#include "core/providers/provider.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include "support/ut.hpp"
#include <thread>

using namespace orangutan;

namespace {

LLMResponse text_response(const std::string &text) {
    return {
        .stop_reason = "end_turn",
        .content = {TextBlock{.text = text}},
    };
}

class StubProvider final : public Provider {
public:
    using ChatHandler = std::function<LLMResponse()>;
    using StreamHandler = std::function<LLMResponse(const StreamCallback &)>;

    StubProvider(std::string provider_name, std::string model, ChatHandler chat_handler, StreamHandler stream_handler)
    : provider_name_(std::move(provider_name)),
      model_(std::move(model)),
      chat_handler_(std::move(chat_handler)),
      stream_handler_(std::move(stream_handler)) {}

    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return chat_handler_();
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &on_event, int) override {
        return stream_handler_(on_event);
    }

    std::string name() const override {
        return provider_name_;
    }

    std::string current_model() const override {
        return model_;
    }

private:
    std::string provider_name_;
    std::string model_;
    ChatHandler chat_handler_;
    StreamHandler stream_handler_;
};

boost::ut::suite provider_fallback_suite = [] {
    using namespace boost::ut;

    "falls_back_to_next_model_and_tracks_usage"_test = [] {
        size_t primary_attempts = 0;
        size_t fallback_attempts = 0;

        auto provider = create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback"},
                                                       [&](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                                           if (endpoint.model == "gpt-primary") {
                                                               return std::make_unique<StubProvider>(
                                                                   endpoint.provider_name, endpoint.model,
                                                                   [&]() -> LLMResponse {
                                                                       ++primary_attempts;
                                                                       throw std::runtime_error("primary unavailable");
                                                                   },
                                                                   [](const StreamCallback &) -> LLMResponse {
                                                                       throw std::runtime_error("stream should not be used");
                                                                   });
                                                           }

                                                           return std::make_unique<StubProvider>(
                                                               endpoint.provider_name, endpoint.model,
                                                               [&]() -> LLMResponse {
                                                                   ++fallback_attempts;
                                                                   return text_response("fallback response");
                                                               },
                                                               [](const StreamCallback &) -> LLMResponse {
                                                                   throw std::runtime_error("stream should not be used");
                                                               });
                                                       });

        const auto response = provider->chat("", {}, {}, 1024);

        expect(response.content.size() == 1_ul);
        const auto *text = std::get_if<TextBlock>(&response.content[0]);
        expect((text != nullptr) >> fatal);
        if (text != nullptr) {
            expect(text->text == "fallback response");
        }
        expect(primary_attempts == 1_ul);
        expect(fallback_attempts == 1_ul);
        expect(provider->current_model() == "gpt-fallback");

        const auto usage = provider->usage();
        expect(usage.logical_requests == 1_ul);
        expect(usage.attempt_count == 2_ul);
        expect(usage.failed_attempts == 1_ul);
        expect(usage.fallback_switches == 1_ul);
    };

    "rejects_missing_api_key_before_instantiating_providers"_test = [] {
        bool factory_called = false;

        auto provider = create_provider_with_fallbacks("openai", "", "gpt-primary", "https://example.test", {"gpt-fallback"},
                                                       [&factory_called](const ProviderEndpoint &) -> std::unique_ptr<Provider> {
                                                           factory_called = true;
                                                           return nullptr;
                                                       });

        try {
            static_cast<void>(provider->chat("", {}, {}, 1024));
            expect(false >> fatal) << "expected missing api key error";
        } catch (const MissingApiKeyError &error) {
            expect(std::string_view{error.what()} == "missing API key for provider 'openai' model 'gpt-primary'");
        }
        expect(not factory_called);

        const auto usage = provider->usage();
        expect(usage.logical_requests == 1_ul);
        expect(usage.attempt_count == 1_ul);
        expect(usage.failed_attempts == 1_ul);
        expect(usage.fallback_switches == 0_ul);
    };

    "does_not_fallback_after_streaming_output_has_started"_test = [] {
        size_t primary_stream_attempts = 0;
        size_t fallback_stream_attempts = 0;
        std::vector<std::string> observed_chunks;

        auto provider = create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback"},
                                                       [&](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                                           if (endpoint.model == "gpt-primary") {
                                                               return std::make_unique<StubProvider>(
                                                                   endpoint.provider_name, endpoint.model,
                                                                   []() -> LLMResponse {
                                                                       throw std::runtime_error("chat should not be used");
                                                                   },
                                                                   [&](const StreamCallback &on_event) -> LLMResponse {
                                                                       ++primary_stream_attempts;
                                                                       on_event("text_delta", json{{"text", "partial"}});
                                                                       throw std::runtime_error("stream interrupted");
                                                                   });
                                                           }

                                                           return std::make_unique<StubProvider>(
                                                               endpoint.provider_name, endpoint.model,
                                                               []() -> LLMResponse {
                                                                   throw std::runtime_error("chat should not be used");
                                                               },
                                                               [&](const StreamCallback &on_event) -> LLMResponse {
                                                                   ++fallback_stream_attempts;
                                                                   on_event("text_delta", json{{"text", "fallback"}});
                                                                   return text_response("fallback response");
                                                               });
                                                       });

        try {
            static_cast<void>(provider->chat_stream(
                "", {}, {},
                [&observed_chunks](const std::string &event_type, const json &data) {
                    if (event_type == "text_delta") {
                        observed_chunks.push_back(data.at("text").get<std::string>());
                    }
                },
                1024));
            expect(false >> fatal) << "expected interrupted primary stream";
        } catch (const std::runtime_error &) {
        }

        expect(observed_chunks.size() == 1_ul);
        expect(observed_chunks[0] == "partial");
        expect(primary_stream_attempts == 1_ul);
        expect(fallback_stream_attempts == 0_ul);
        expect(provider->current_model() == "gpt-primary");

        const auto usage = provider->usage();
        expect(usage.logical_requests == 1_ul);
        expect(usage.attempt_count == 1_ul);
        expect(usage.failed_attempts == 1_ul);
        expect(usage.fallback_switches == 0_ul);
    };

    "keeps_primary_provider_alive_while_in_flight_request_completes"_test = [] {
        struct PrimaryState {
            std::atomic<size_t> call_count{0};
            std::atomic<bool> started{false};
            std::atomic<bool> destroyed{false};
            std::atomic<bool> destructor_signaled{false};
            std::promise<void> first_call_started;
            std::promise<void> allow_first_call_to_finish;
            std::promise<void> destroyed_promise;
            std::shared_future<void> allow_first_call_future = allow_first_call_to_finish.get_future().share();
        };

        auto primary_state = std::make_shared<PrimaryState>();

        class ConcurrentPrimaryProvider final : public Provider {
        public:
            ConcurrentPrimaryProvider(std::string provider_name, std::string model, std::shared_ptr<PrimaryState> state)
            : provider_name_(std::move(provider_name)),
              model_(std::move(model)),
              state_(std::move(state)) {}

            ~ConcurrentPrimaryProvider() override {
                if (state_ != nullptr) {
                    state_->destroyed = true;
                    if (!state_->destructor_signaled.exchange(true)) {
                        state_->destroyed_promise.set_value();
                    }
                }
            }

            ConcurrentPrimaryProvider(const ConcurrentPrimaryProvider &) = delete;
            ConcurrentPrimaryProvider &operator=(const ConcurrentPrimaryProvider &) = delete;
            ConcurrentPrimaryProvider(ConcurrentPrimaryProvider &&) = delete;
            ConcurrentPrimaryProvider &operator=(ConcurrentPrimaryProvider &&) = delete;

            LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
                auto state = state_;
                const auto call_number = ++state->call_count;
                if (call_number == 1) {
                    if (!state->started.exchange(true)) {
                        state->first_call_started.set_value();
                    }
                    state->allow_first_call_future.wait();
                    return text_response("primary response");
                }

                throw std::runtime_error("primary unavailable");
            }

            LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
                throw std::runtime_error("stream should not be used");
            }

            std::string name() const override {
                return provider_name_;
            }

            std::string current_model() const override {
                return model_;
            }

        private:
            std::string provider_name_;
            std::string model_;
            std::shared_ptr<PrimaryState> state_;
        };

        auto provider = create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback"},
                                                       [primary_state](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                                           if (endpoint.model == "gpt-primary") {
                                                               return std::make_unique<ConcurrentPrimaryProvider>(endpoint.provider_name, endpoint.model, primary_state);
                                                           }

                                                           return std::make_unique<StubProvider>(
                                                               endpoint.provider_name, endpoint.model,
                                                               []() -> LLMResponse {
                                                                   return text_response("fallback response");
                                                               },
                                                               [](const StreamCallback &) -> LLMResponse {
                                                                   throw std::runtime_error("stream should not be used");
                                                               });
                                                       });

        auto first_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });

        expect(primary_state->first_call_started.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        auto second_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });

        expect(second_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto fallback_response = second_result.get();
        expect(fallback_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(fallback_response.content[0]).text == "fallback response");

        expect(not primary_state->destroyed.load());

        primary_state->allow_first_call_to_finish.set_value();
        expect(first_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto primary_response = first_result.get();
        expect(primary_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(primary_response.content[0]).text == "primary response");

        expect(primary_state->destroyed_promise.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(primary_state->destroyed.load());

        const auto usage = provider->usage();
        expect(usage.logical_requests == 2_ul);
        expect(usage.attempt_count == 3_ul);
        expect(usage.failed_attempts == 1_ul);
        expect(usage.fallback_switches == 1_ul);
    };

    "late_primary_failure_still_falls_back_after_another_request_switches_models"_test = [] {
        struct PrimaryState {
            std::promise<void> first_call_started;
            std::promise<void> release_first_failure;
            std::shared_future<void> release_first_failure_future = release_first_failure.get_future().share();
            std::atomic<size_t> call_count{0};
        };

        auto primary_state = std::make_shared<PrimaryState>();

        class DelayedFailingPrimaryProvider final : public Provider {
        public:
            DelayedFailingPrimaryProvider(std::string provider_name, std::string model, std::shared_ptr<PrimaryState> state)
            : provider_name_(std::move(provider_name)),
              model_(std::move(model)),
              state_(std::move(state)) {}

            LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
                const auto call_number = ++state_->call_count;
                if (call_number == 1) {
                    state_->first_call_started.set_value();
                    state_->release_first_failure_future.wait();
                }

                throw std::runtime_error("primary unavailable");
            }

            LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
                throw std::runtime_error("stream should not be used");
            }

            std::string name() const override {
                return provider_name_;
            }

            std::string current_model() const override {
                return model_;
            }

        private:
            std::string provider_name_;
            std::string model_;
            std::shared_ptr<PrimaryState> state_;
        };

        std::atomic<size_t> fallback_attempts{0};
        auto provider = create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback"},
                                                       [primary_state, &fallback_attempts](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                                           if (endpoint.model == "gpt-primary") {
                                                               return std::make_unique<DelayedFailingPrimaryProvider>(endpoint.provider_name, endpoint.model, primary_state);
                                                           }

                                                           return std::make_unique<StubProvider>(
                                                               endpoint.provider_name, endpoint.model,
                                                               [&fallback_attempts]() -> LLMResponse {
                                                                   ++fallback_attempts;
                                                                   return text_response("fallback response");
                                                               },
                                                               [](const StreamCallback &) -> LLMResponse {
                                                                   throw std::runtime_error("stream should not be used");
                                                               });
                                                       });

        auto first_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });
        expect(primary_state->first_call_started.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        auto second_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });
        expect(second_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto second_response = second_result.get();
        expect(second_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(second_response.content[0]).text == "fallback response");

        primary_state->release_first_failure.set_value();
        expect(first_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto first_response = first_result.get();
        expect(first_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(first_response.content[0]).text == "fallback response");
        expect(fallback_attempts.load() == 2_ul);

        const auto usage = provider->usage();
        expect(usage.logical_requests == 2_ul);
        expect(usage.attempt_count == 4_ul);
        expect(usage.failed_attempts == 2_ul);
        expect(usage.fallback_switches == 1_ul);
    };

    "concurrent_primary_failures_do_not_skip_fallback_models"_test = [] {
        struct PrimaryFailureState {
            std::atomic<size_t> call_count{0};
            std::atomic<bool> both_calls_signaled{false};
            std::promise<void> both_calls_started;
            std::promise<void> release_failures;
            std::shared_future<void> release_failures_future = release_failures.get_future().share();
        };

        class CoordinatedFailingPrimaryProvider final : public Provider {
        public:
            CoordinatedFailingPrimaryProvider(std::string provider_name, std::string model, std::shared_ptr<PrimaryFailureState> state)
            : provider_name_(std::move(provider_name)),
              model_(std::move(model)),
              state_(std::move(state)) {}

            LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
                const auto started = ++state_->call_count;
                if (started == 2 && !state_->both_calls_signaled.exchange(true)) {
                    state_->both_calls_started.set_value();
                }
                while (state_->call_count.load() < 2) {
                    std::this_thread::yield();
                }
                state_->release_failures_future.wait();
                throw std::runtime_error("primary unavailable");
            }

            LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
                throw std::runtime_error("stream should not be used");
            }

            std::string name() const override {
                return provider_name_;
            }

            std::string current_model() const override {
                return model_;
            }

        private:
            std::string provider_name_;
            std::string model_;
            std::shared_ptr<PrimaryFailureState> state_;
        };

        auto primary_state = std::make_shared<PrimaryFailureState>();
        std::atomic<size_t> first_fallback_attempts{0};
        std::atomic<size_t> second_fallback_attempts{0};

        auto provider =
            create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback-a", "gpt-fallback-b"},
                                           [primary_state, &first_fallback_attempts, &second_fallback_attempts](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                               if (endpoint.model == "gpt-primary") {
                                                   return std::make_unique<CoordinatedFailingPrimaryProvider>(endpoint.provider_name, endpoint.model, primary_state);
                                               }

                                               if (endpoint.model == "gpt-fallback-a") {
                                                   return std::make_unique<StubProvider>(
                                                       endpoint.provider_name, endpoint.model,
                                                       [&first_fallback_attempts]() -> LLMResponse {
                                                           ++first_fallback_attempts;
                                                           return text_response("fallback-a");
                                                       },
                                                       [](const StreamCallback &) -> LLMResponse {
                                                           throw std::runtime_error("stream should not be used");
                                                       });
                                               }

                                               return std::make_unique<StubProvider>(
                                                   endpoint.provider_name, endpoint.model,
                                                   [&second_fallback_attempts]() -> LLMResponse {
                                                       ++second_fallback_attempts;
                                                       return text_response("fallback-b");
                                                   },
                                                   [](const StreamCallback &) -> LLMResponse {
                                                       throw std::runtime_error("stream should not be used");
                                                   });
                                           });

        auto first_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });
        auto second_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });

        expect(primary_state->both_calls_started.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        primary_state->release_failures.set_value();

        expect(first_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(second_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        const auto first_response = first_result.get();
        const auto second_response = second_result.get();
        expect(first_response.content.size() == 1_ul);
        expect(second_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(first_response.content[0]).text == "fallback-a");
        expect(std::get<TextBlock>(second_response.content[0]).text == "fallback-a");
        expect(first_fallback_attempts.load() == 2_ul);
        expect(second_fallback_attempts.load() == 0_ul);
        expect(provider->current_model() == "gpt-fallback-a");

        const auto usage = provider->usage();
        expect(usage.logical_requests == 2_ul);
        expect(usage.attempt_count == 4_ul);
        expect(usage.failed_attempts == 2_ul);
        expect(usage.fallback_switches == 1_ul);
    };

    "late_primary_failure_keeps_its_own_fallback_order"_test = [] {
        struct PrimaryState {
            std::promise<void> second_call_started;
            std::shared_future<void> second_call_started_future = second_call_started.get_future().share();
            std::promise<void> release_second_failure;
            std::shared_future<void> release_second_failure_future = release_second_failure.get_future().share();
            std::atomic<size_t> call_count{0};
        };

        auto primary_state = std::make_shared<PrimaryState>();
        std::atomic<size_t> fallback_a_attempts{0};
        std::atomic<size_t> fallback_b_attempts{0};

        class OrderedFailingPrimaryProvider final : public Provider {
        public:
            OrderedFailingPrimaryProvider(std::string provider_name, std::string model, std::shared_ptr<PrimaryState> state)
            : provider_name_(std::move(provider_name)),
              model_(std::move(model)),
              state_(std::move(state)) {}

            LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
                const auto call_number = ++state_->call_count;
                if (call_number == 1) {
                    state_->second_call_started_future.wait();
                } else {
                    state_->second_call_started.set_value();
                    state_->release_second_failure_future.wait();
                }
                throw std::runtime_error("primary unavailable");
            }

            LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
                throw std::runtime_error("stream should not be used");
            }

            std::string name() const override {
                return provider_name_;
            }

            std::string current_model() const override {
                return model_;
            }

        private:
            std::string provider_name_;
            std::string model_;
            std::shared_ptr<PrimaryState> state_;
        };

        auto provider = create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback-a", "gpt-fallback-b"},
                                                       [primary_state, &fallback_a_attempts, &fallback_b_attempts](const ProviderEndpoint &endpoint) -> std::unique_ptr<Provider> {
                                                           if (endpoint.model == "gpt-primary") {
                                                               return std::make_unique<OrderedFailingPrimaryProvider>(endpoint.provider_name, endpoint.model, primary_state);
                                                           }

                                                           if (endpoint.model == "gpt-fallback-a") {
                                                               return std::make_unique<StubProvider>(
                                                                   endpoint.provider_name, endpoint.model,
                                                                   [&fallback_a_attempts]() -> LLMResponse {
                                                                       if (++fallback_a_attempts == 1) {
                                                                           throw std::runtime_error("fallback-a unavailable");
                                                                       }
                                                                       return text_response("fallback-a");
                                                                   },
                                                                   [](const StreamCallback &) -> LLMResponse {
                                                                       throw std::runtime_error("stream should not be used");
                                                                   });
                                                           }

                                                           return std::make_unique<StubProvider>(
                                                               endpoint.provider_name, endpoint.model,
                                                               [&fallback_b_attempts]() -> LLMResponse {
                                                                   ++fallback_b_attempts;
                                                                   return text_response("fallback-b");
                                                               },
                                                               [](const StreamCallback &) -> LLMResponse {
                                                                   throw std::runtime_error("stream should not be used");
                                                               });
                                                       });

        auto first_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });
        auto second_result = std::async(std::launch::async, [&provider] {
            return provider->chat("", {}, {}, 1024);
        });

        expect(primary_state->second_call_started_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(first_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        const auto first_response = first_result.get();
        expect(first_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(first_response.content[0]).text == "fallback-b");

        primary_state->release_second_failure.set_value();
        expect(second_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto second_response = second_result.get();
        expect(second_response.content.size() == 1_ul);
        expect(std::get<TextBlock>(second_response.content[0]).text == "fallback-a");

        expect(fallback_a_attempts.load() == 2_ul);
        expect(fallback_b_attempts.load() == 1_ul);
        expect(provider->current_model() == "gpt-fallback-b");

        const auto usage = provider->usage();
        expect(usage.logical_requests == 2_ul);
        expect(usage.attempt_count == 5_ul);
        expect(usage.failed_attempts == 3_ul);
        expect(usage.fallback_switches == 2_ul);
    };
};

} // namespace
