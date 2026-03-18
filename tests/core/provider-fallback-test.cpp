#include "core/providers/provider.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
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

} // namespace

TEST(ProviderFallbackTest, FallsBackToNextModelAndTracksUsage) {
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

    ASSERT_EQ(response.content.size(), 1U);
    const auto *text = std::get_if<TextBlock>(&response.content[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "fallback response");
    EXPECT_EQ(primary_attempts, 1U);
    EXPECT_EQ(fallback_attempts, 1U);
    EXPECT_EQ(provider->current_model(), "gpt-fallback");

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 1U);
    EXPECT_EQ(usage.attempt_count, 2U);
    EXPECT_EQ(usage.failed_attempts, 1U);
    EXPECT_EQ(usage.fallback_switches, 1U);
}

TEST(ProviderFallbackTest, DoesNotFallbackAfterStreamingOutputHasStarted) {
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

    EXPECT_THROW(provider->chat_stream(
                     "", {}, {},
                     [&observed_chunks](const std::string &event_type, const json &data) {
                         if (event_type == "text_delta") {
                             observed_chunks.push_back(data.at("text").get<std::string>());
                         }
                     },
                     1024),
                 std::runtime_error);

    ASSERT_EQ(observed_chunks.size(), 1U);
    EXPECT_EQ(observed_chunks[0], "partial");
    EXPECT_EQ(primary_stream_attempts, 1U);
    EXPECT_EQ(fallback_stream_attempts, 0U);
    EXPECT_EQ(provider->current_model(), "gpt-primary");

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 1U);
    EXPECT_EQ(usage.attempt_count, 1U);
    EXPECT_EQ(usage.failed_attempts, 1U);
    EXPECT_EQ(usage.fallback_switches, 0U);
}

TEST(ProviderFallbackTest, KeepsPrimaryProviderAliveWhileInFlightRequestCompletes) {
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

    ASSERT_EQ(primary_state->first_call_started.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    auto second_result = std::async(std::launch::async, [&provider] {
        return provider->chat("", {}, {}, 1024);
    });

    ASSERT_EQ(second_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto fallback_response = second_result.get();
    ASSERT_EQ(fallback_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(fallback_response.content[0]).text, "fallback response");

    EXPECT_FALSE(primary_state->destroyed.load());

    primary_state->allow_first_call_to_finish.set_value();
    ASSERT_EQ(first_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto primary_response = first_result.get();
    ASSERT_EQ(primary_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(primary_response.content[0]).text, "primary response");

    ASSERT_EQ(primary_state->destroyed_promise.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(primary_state->destroyed.load());

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 2U);
    EXPECT_EQ(usage.attempt_count, 3U);
    EXPECT_EQ(usage.failed_attempts, 1U);
    EXPECT_EQ(usage.fallback_switches, 1U);
}

TEST(ProviderFallbackTest, LatePrimaryFailureStillFallsBackAfterAnotherRequestSwitchesModels) {
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
    auto provider = create_provider_with_fallbacks(
        "openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback"},
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
    ASSERT_EQ(primary_state->first_call_started.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);

    auto second_result = std::async(std::launch::async, [&provider] {
        return provider->chat("", {}, {}, 1024);
    });
    ASSERT_EQ(second_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto second_response = second_result.get();
    ASSERT_EQ(second_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(second_response.content[0]).text, "fallback response");

    primary_state->release_first_failure.set_value();
    ASSERT_EQ(first_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto first_response = first_result.get();
    ASSERT_EQ(first_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(first_response.content[0]).text, "fallback response");
    EXPECT_EQ(fallback_attempts.load(), 2U);

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 2U);
    EXPECT_EQ(usage.attempt_count, 4U);
    EXPECT_EQ(usage.failed_attempts, 2U);
    EXPECT_EQ(usage.fallback_switches, 1U);
}

TEST(ProviderFallbackTest, ConcurrentPrimaryFailuresDoNotSkipFallbackModels) {
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

    ASSERT_EQ(primary_state->both_calls_started.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
    primary_state->release_failures.set_value();

    ASSERT_EQ(first_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_EQ(second_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    const auto first_response = first_result.get();
    const auto second_response = second_result.get();
    ASSERT_EQ(first_response.content.size(), 1U);
    ASSERT_EQ(second_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(first_response.content[0]).text, "fallback-a");
    EXPECT_EQ(std::get<TextBlock>(second_response.content[0]).text, "fallback-a");
    EXPECT_EQ(first_fallback_attempts.load(), 2U);
    EXPECT_EQ(second_fallback_attempts.load(), 0U);
    EXPECT_EQ(provider->current_model(), "gpt-fallback-a");

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 2U);
    EXPECT_EQ(usage.attempt_count, 4U);
    EXPECT_EQ(usage.failed_attempts, 2U);
    EXPECT_EQ(usage.fallback_switches, 1U);
}

TEST(ProviderFallbackTest, LatePrimaryFailureKeepsItsOwnFallbackOrder) {
    struct PrimaryState {
        std::promise<void> second_call_started;
        std::shared_future<void> second_call_started_future = second_call_started.get_future().share();
        std::promise<void> release_second_failure;
        std::shared_future<void> release_second_failure_future = release_second_failure.get_future().share();
        std::atomic<size_t> call_count{0};
    };

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

    auto primary_state = std::make_shared<PrimaryState>();
    std::atomic<size_t> fallback_a_attempts{0};
    std::atomic<size_t> fallback_b_attempts{0};

    auto provider =
        create_provider_with_fallbacks("openai", "unused", "gpt-primary", "https://example.test", {"gpt-fallback-a", "gpt-fallback-b"},
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

    ASSERT_EQ(primary_state->second_call_started_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_EQ(first_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    const auto first_response = first_result.get();
    ASSERT_EQ(first_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(first_response.content[0]).text, "fallback-b");

    primary_state->release_second_failure.set_value();
    ASSERT_EQ(second_result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto second_response = second_result.get();
    ASSERT_EQ(second_response.content.size(), 1U);
    EXPECT_EQ(std::get<TextBlock>(second_response.content[0]).text, "fallback-a");

    EXPECT_EQ(fallback_a_attempts.load(), 2U);
    EXPECT_EQ(fallback_b_attempts.load(), 1U);
    EXPECT_EQ(provider->current_model(), "gpt-fallback-b");

    const auto usage = provider->usage();
    EXPECT_EQ(usage.logical_requests, 2U);
    EXPECT_EQ(usage.attempt_count, 5U);
    EXPECT_EQ(usage.failed_attempts, 3U);
    EXPECT_EQ(usage.fallback_switches, 2U);
}
