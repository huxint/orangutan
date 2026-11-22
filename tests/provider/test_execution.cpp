// tests/provider/test_execution.cpp — provider execution retry/fallback coverage.

#include <oran/provider.hpp>

#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

namespace {

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace prov = orangutan::provider;
namespace test = orangutan::tests;

prov::ModelTarget target(std::string profile, std::string model) {
  return prov::ModelTarget{
      .profile = std::move(profile),
      .model = std::move(model),
      .protocol = prov::ProtocolKind::anthropic_messages,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

prov::Route route_with_fallback() {
  return prov::Route{
      .primary = target("primary-profile", "primary-model"),
      .fallbacks = {target("fallback-profile", "fallback-model")},
  };
}

prov::Response text_response(std::string text, std::optional<std::string> model_used = std::nullopt) {
  return prov::Response{
      .blocks = {core::TextContent{.text = std::move(text)}},
      .stop_reason = core::StopReason::end_turn,
      .usage = {},
      .model_used = std::move(model_used),
  };
}

prov::Request request_with_retry(std::uint32_t max_attempts, std::chrono::milliseconds backoff = 0ms) {
  auto request = prov::Request{};
  request.retry = prov::RetryPolicy{
      .max_attempts = max_attempts,
      .initial_backoff = backoff,
  };
  return request;
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

class RecordingSystem final : public prov::System {
public:
  explicit RecordingSystem(std::vector<core::Result<prov::Response>> plan) : plan_{std::move(plan)} {}

  [[nodiscard]] async::Awaitable<core::Result<prov::Response>>
  send(prov::Request request, prov::ModelTarget target, prov::EventSink* sink = nullptr) const override {
    static_cast<void>(sink);
    requests_seen_.push_back(request);
    targets_seen_.push_back(target);

    const auto cursor = cursor_++;
    if (cursor >= plan_.size()) {
      co_return std::unexpected(core::Error::internal("recording provider exhausted"));
    }

    const auto& scripted = plan_[cursor];
    if (scripted.has_value()) {
      co_return *scripted;
    }
    co_return std::unexpected(scripted.error());
  }

  [[nodiscard]] const std::vector<prov::ModelTarget>& targets_seen() const noexcept {
    return targets_seen_;
  }

  [[nodiscard]] const std::vector<prov::Request>& requests_seen() const noexcept {
    return requests_seen_;
  }

private:
  std::vector<core::Result<prov::Response>> plan_;
  mutable std::vector<prov::ModelTarget> targets_seen_;
  mutable std::vector<prov::Request> requests_seen_;
  mutable std::size_t cursor_{0};
};

class StreamingFailureSystem final : public prov::System {
public:
  [[nodiscard]] async::Awaitable<core::Result<prov::Response>>
  send(prov::Request request, prov::ModelTarget target, prov::EventSink* sink = nullptr) const override {
    static_cast<void>(request);
    targets_seen_.push_back(target);
    if (sink != nullptr) {
      sink->on_text_delta("partial");
    }
    co_return std::unexpected(core::Error::network("stream failed after visible output"));
  }

  [[nodiscard]] const std::vector<prov::ModelTarget>& targets_seen() const noexcept {
    return targets_seen_;
  }

private:
  mutable std::vector<prov::ModelTarget> targets_seen_;
};

class TextSink final : public prov::EventSink {
public:
  void on_text_delta(std::string_view delta) override {
    text.append(delta);
  }

  std::string text;
};

// Streams nothing on the first attempt (a pre-first-byte failure), then streams
// a delta and succeeds on the second. Retried deltas still reach the caller.
class StreamingRetrySystem final : public prov::System {
public:
  [[nodiscard]] async::Awaitable<core::Result<prov::Response>>
  send(prov::Request request, prov::ModelTarget target, prov::EventSink* sink = nullptr) const override {
    static_cast<void>(request);
    targets_seen_.push_back(target);
    if (cursor_++ == 0) {
      // First attempt fails before emitting any visible output.
      co_return std::unexpected(core::Error::network("transient failure before first byte"));
    }
    if (sink != nullptr) {
      sink->on_text_delta("recovered");
    }
    co_return text_response("recovered");
  }

  [[nodiscard]] const std::vector<prov::ModelTarget>& targets_seen() const noexcept {
    return targets_seen_;
  }

private:
  mutable std::vector<prov::ModelTarget> targets_seen_;
  mutable std::size_t cursor_{0};
};

}  // namespace

TEST_CASE("Provider execution retries retryable errors on the same target", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error::network("temporary network failure")),
        text_response("ok"),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE(result.response.has_value());
    REQUIRE(std::get<core::TextContent>(result.response->blocks.front()).text == "ok");
    REQUIRE(result.target.model == std::string{"primary-model"});
    REQUIRE(backend.targets_seen().size() == 2);
    REQUIRE(backend.targets_seen()[0].model == "primary-model");
    REQUIRE(backend.targets_seen()[1].model == "primary-model");
  });
}

TEST_CASE("Provider execution falls back after retryable primary exhaustion", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error::network("first primary failure")),
        std::unexpected(core::Error::upstream("second primary failure")),
        text_response("fallback ok"),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE(result.response.has_value());
    REQUIRE(std::get<core::TextContent>(result.response->blocks.front()).text == "fallback ok");
    REQUIRE(result.target.model == std::string{"fallback-model"});
    REQUIRE(backend.targets_seen().size() == 3);
    REQUIRE(backend.targets_seen()[0].model == "primary-model");
    REQUIRE(backend.targets_seen()[1].model == "primary-model");
    REQUIRE(backend.targets_seen()[2].model == "fallback-model");
  });
}

TEST_CASE("Provider execution separates reported models from target attribution", "[unit][provider][execution]") {
  std::optional<std::string> reported_model;
  std::string attributed_model{"primary-model"};
  SECTION("omitted model") {}
  SECTION("empty model") {
    reported_model = std::string{};
  }
  SECTION("reported model") {
    reported_model = "adapter-reported-model";
    attributed_model = *reported_model;
  }

  test::run_async([reported_model, attributed_model](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        text_response("ok", reported_model),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(1), route_with_fallback(), nullptr);

    REQUIRE(result.response.has_value());
    CHECK(result.response->model_used == reported_model);
    CHECK(result.target.model == attributed_model);
  });
}

TEST_CASE("Provider execution stops on non-retryable errors", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error{core::ErrorKind::auth, "bad key"}),
        text_response("should not run"),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.response.has_value());
    REQUIRE(result.response.error().kind() == core::ErrorKind::auth);
    REQUIRE(result.target.profile == "primary-profile");
    REQUIRE(result.target.model == "primary-model");
    REQUIRE(context_value(result.response.error(), "attempt") == std::optional<std::string_view>{"1"});
    REQUIRE(backend.targets_seen().size() == 1);
  });
}

TEST_CASE("Provider execution does not retry after visible stream output", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    StreamingFailureSystem backend;
    TextSink sink;

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), &sink);

    REQUIRE_FALSE(result.response.has_value());
    REQUIRE(result.response.error().kind() == core::ErrorKind::network);
    REQUIRE(context_value(result.response.error(), "retry_skipped") ==
            std::optional<std::string_view>{"stream_already_emitted"});
    REQUIRE(context_value(result.response.error(), "fallback_skipped") ==
            std::optional<std::string_view>{"stream_already_emitted"});
    REQUIRE(sink.text == "partial");
    REQUIRE(backend.targets_seen().size() == 1);
    REQUIRE(backend.targets_seen()[0].model == "primary-model");
  });
}

TEST_CASE("Provider execution retries a streaming failure that emitted nothing", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    StreamingRetrySystem backend;
    TextSink sink;

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), &sink);

    REQUIRE(result.response.has_value());
    REQUIRE(std::get<core::TextContent>(result.response->blocks.front()).text == "recovered");
    REQUIRE(sink.text == "recovered");
    REQUIRE(backend.targets_seen().size() == 2);
    REQUIRE(backend.targets_seen()[0].model == "primary-model");
    REQUIRE(backend.targets_seen()[1].model == "primary-model");
  });
}

TEST_CASE("Provider execution rejects zero retry attempts before sending", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        text_response("should not run"),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(0), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.response.has_value());
    REQUIRE(result.response.error().kind() == core::ErrorKind::invalid_argument);
    CHECK(result.target.profile == "primary-profile");
    CHECK(result.target.model == "primary-model");
    CHECK(result.target.protocol == prov::ProtocolKind::anthropic_messages);
    CHECK_FALSE(result.target.fallback);
    REQUIRE(backend.targets_seen().empty());
  });
}

TEST_CASE("Provider execution observes cancellation during retry backoff", "[unit][provider][execution]") {
  asio::io_context io;
  asio::cancellation_signal signal;
  RecordingSystem backend{std::vector<core::Result<prov::Response>>{
      std::unexpected(core::Error::network("temporary network failure")),
      text_response("should not run"),
  }};

  std::optional<prov::execution::Outcome> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<prov::execution::Outcome> {
        co_return co_await prov::execution::run(backend, request_with_retry(2, 1s), route_with_fallback(), nullptr);
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, prov::execution::Outcome r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE(result->target.profile == "primary-profile");
  REQUIRE_FALSE(result->target.fallback);
  REQUIRE_FALSE(result->response.has_value());
  REQUIRE(result->response.error().kind() == core::ErrorKind::cancelled);
  REQUIRE(result->target.model == "primary-model");
  REQUIRE(context_value(result->response.error(), "attempt") == std::optional<std::string_view>{"1"});
  REQUIRE(context_value(result->response.error(), "max_attempts") == std::optional<std::string_view>{"2"});
  REQUIRE(backend.targets_seen().size() == 1);
}

TEST_CASE("Provider execution attributes terminal fallback errors to the fallback target",
          "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error::network("first primary failure")),
        std::unexpected(core::Error::upstream("second primary failure")),
        std::unexpected(core::Error{core::ErrorKind::auth, "fallback key rejected"}),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.response.has_value());
    REQUIRE(result.response.error().kind() == core::ErrorKind::auth);
    REQUIRE(result.target.profile == "fallback-profile");
    REQUIRE(result.target.model == "fallback-model");
    REQUIRE(context_value(result.response.error(), "route_role") == std::optional<std::string_view>{"fallback"});
    REQUIRE(backend.targets_seen().size() == 3);
    REQUIRE(backend.targets_seen()[2].profile == "fallback-profile");
  });
}

TEST_CASE("Provider execution marks primary errors with route_role primary", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSystem backend{std::vector<core::Result<prov::Response>>{
        std::unexpected(core::Error{core::ErrorKind::auth, "bad key"}),
    }};

    auto result = co_await prov::execution::run(backend, request_with_retry(1), route_with_fallback(), nullptr);

    REQUIRE_FALSE(result.response.has_value());
    REQUIRE(result.target.profile == "primary-profile");
    REQUIRE(context_value(result.response.error(), "route_role") == std::optional<std::string_view>{"primary"});
  });
}

TEST_CASE("Provider execution applies per-target thinking policy to fallback attempts", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SECTION("strips a budget a fallback protocol cannot carry") {
      auto primary = target("primary-profile", "primary-model");
      auto fallback = target("fallback-profile", "fallback-model");
      fallback.protocol = prov::ProtocolKind::openai_responses;
      auto route = prov::Route{.primary = primary, .fallbacks = {fallback}};

      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};

      auto request = request_with_retry(2);
      request.thinking_budget = 1024;  // folded from the primary profile by the loop
      auto result = co_await prov::execution::run(backend, std::move(request), route, nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(backend.requests_seen().size() == 3);
      REQUIRE(backend.requests_seen()[0].thinking_budget == std::optional<std::uint32_t>{1024});
      REQUIRE(backend.requests_seen()[1].thinking_budget == std::optional<std::uint32_t>{1024});
      // openai_responses rejects token-budget thinking controls; the fallback
      // attempt must not carry the primary's budget.
      REQUIRE_FALSE(backend.requests_seen()[2].thinking_budget.has_value());
    }
    SECTION("applies an explicit fallback profile budget") {
      auto primary = target("primary-profile", "primary-model");
      auto fallback = target("fallback-profile", "fallback-model");
      fallback.thinking_budget = 512;
      auto route = prov::Route{.primary = primary, .fallbacks = {fallback}};

      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};

      auto request = request_with_retry(2);
      auto result = co_await prov::execution::run(backend, std::move(request), route, nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(backend.requests_seen()[2].thinking_budget == std::optional<std::uint32_t>{512});
    }
    co_return;
  });
}

TEST_CASE("Provider execution preserves cache hints for protocol policy", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SECTION("disabled targets retain the prefix for protocol encoding") {
      auto primary = target("primary-profile", "primary-model");
      auto fallback = target("fallback-profile", "fallback-model");
      fallback.cache = prov::PromptCacheOptions{.enabled = false, .min_prefix_bytes = 0};
      auto route = prov::Route{.primary = primary, .fallbacks = {fallback}};

      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};

      auto request = request_with_retry(2);
      request.cache = prov::PromptCacheHints{.prefix_bytes = 4096};
      auto result = co_await prov::execution::run(backend, std::move(request), route, nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(backend.requests_seen()[0].cache.has_value());
      REQUIRE(backend.requests_seen()[1].cache.has_value());
      REQUIRE(backend.requests_seen()[2].cache == backend.requests_seen()[0].cache);
    }
    SECTION("hints below the target floor reach the protocol encoder") {
      auto primary = target("primary-profile", "primary-model");
      auto fallback = target("fallback-profile", "fallback-model");
      fallback.cache = prov::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1000};
      auto route = prov::Route{.primary = primary, .fallbacks = {fallback}};

      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};

      auto request = request_with_retry(2);
      request.cache = prov::PromptCacheHints{.prefix_bytes = 100};
      auto result = co_await prov::execution::run(backend, std::move(request), route, nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(backend.requests_seen()[0].cache.has_value());
      REQUIRE(backend.requests_seen()[2].cache == backend.requests_seen()[0].cache);
    }
    co_return;
  });
}

TEST_CASE("Provider execution owns primary and fallback attribution", "[unit][provider][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    SECTION("primary success") {
      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          text_response("primary ok"),
      }};

      auto result = co_await prov::execution::run(backend, request_with_retry(1), route_with_fallback(), nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(result.target.profile == "primary-profile");
      REQUIRE_FALSE(result.target.fallback);
    }
    SECTION("fallback success") {
      RecordingSystem backend{std::vector<core::Result<prov::Response>>{
          std::unexpected(core::Error::network("first primary failure")),
          std::unexpected(core::Error::upstream("second primary failure")),
          text_response("fallback ok"),
      }};

      auto result = co_await prov::execution::run(backend, request_with_retry(2), route_with_fallback(), nullptr);

      REQUIRE(result.response.has_value());
      REQUIRE(result.target.model == std::string{"fallback-model"});
      REQUIRE(result.target.profile == "fallback-profile");
      REQUIRE(result.target.fallback);
    }
    co_return;
  });
}

TEST_CASE("Provider execution prices the selected profile and preserves supplied cost",
          "[unit][provider][execution][pricing]") {
  bool fallback = false;
  bool no_pricing = false;
  bool input_only = false;
  std::optional<double> supplied_cost;
  SECTION("primary profile") {}
  SECTION("fallback profile") {
    fallback = true;
  }
  SECTION("input-only pricing") {
    input_only = true;
  }
  SECTION("unpriced profile") {
    no_pricing = true;
  }
  SECTION("reported cost") {
    supplied_cost = 0.125;
  }
  SECTION("reported zero cost") {
    supplied_cost = 0.0;
  }

  test::run_async([fallback, no_pricing, input_only, supplied_cost](asio::io_context&) -> async::Awaitable<void> {
    auto route = route_with_fallback();
    route.primary.pricing = {.input_per_million_usd = 1.0,
                             .output_per_million_usd = 2.0,
                             .cache_creation_per_million_usd = 3.0,
                             .cache_read_per_million_usd = 0.5};
    route.fallbacks[0].pricing = {.input_per_million_usd = 10.0,
                                  .output_per_million_usd = 20.0,
                                  .cache_creation_per_million_usd = 30.0,
                                  .cache_read_per_million_usd = 5.0};
    route.fallbacks[0].protocol = prov::ProtocolKind::openai_responses;
    if (no_pricing) {
      route.primary.pricing = {};
    } else if (input_only) {
      route.primary.pricing = {.input_per_million_usd = 1.0};
    }
    auto response = text_response("priced", std::string{"reported-model"});
    response.usage = {.input_tokens = 100,
                      .output_tokens = 10,
                      .cache_creation_tokens = 20,
                      .cache_read_tokens = 40,
                      .cost_estimate = supplied_cost};
    std::vector<core::Result<prov::Response>> plan;
    if (fallback) {
      plan.emplace_back(std::unexpected(core::Error::network("primary unavailable")));
    }
    plan.emplace_back(std::move(response));
    RecordingSystem backend{std::move(plan)};
    const auto outcome = co_await prov::execution::run(backend, request_with_retry(1), std::move(route));

    REQUIRE(outcome.response.has_value());
    CHECK(outcome.target.profile == (fallback ? "fallback-profile" : "primary-profile"));
    CHECK(outcome.target.model == "reported-model");
    CHECK(outcome.target.protocol ==
          (fallback ? prov::ProtocolKind::openai_responses : prov::ProtocolKind::anthropic_messages));
    CHECK(outcome.target.fallback == fallback);
    const auto cost = outcome.response->usage.cost_estimate;
    if (no_pricing) {
      REQUIRE_FALSE(cost.has_value());
    } else {
      REQUIRE(cost.has_value());
      const auto expected = supplied_cost.value_or(input_only ? 0.00017 : (fallback ? 0.002 : 0.0002));
      CHECK(*cost == Catch::Approx(expected));
    }
  });
}

TEST_CASE("Provider execution returns fallback attribution when a backend throws",
          "[unit][provider][execution][cancellation]") {
  enum class Failure {
    standard,
    nonstandard,
    cancellation,
  };
  auto failure = Failure::standard;
  SECTION("unexpected exception") {}
  SECTION("non-standard exception") {
    failure = Failure::nonstandard;
  }
  SECTION("Asio cancellation") {
    failure = Failure::cancellation;
  }

  test::run_async([failure](asio::io_context&) -> async::Awaitable<void> {
    class ThrowingSystem final : public prov::System {
    public:
      explicit ThrowingSystem(Failure failure) : failure_{failure} {}

      async::Awaitable<core::Result<prov::Response>>
      send(prov::Request, prov::ModelTarget target, prov::EventSink*) const override {
        if (target.profile == "primary-profile") {
          co_return std::unexpected(core::Error::network("primary unavailable"));
        }
        if (failure_ == Failure::cancellation) {
          throw std::system_error(asio::error::make_error_code(asio::error::operation_aborted));
        }
        if (failure_ == Failure::nonstandard) {
          throw failure_;
        }
        throw std::runtime_error("private backend exception details");
      }

    private:
      Failure failure_;
    };
    ThrowingSystem backend{failure};
    auto route = route_with_fallback();
    route.fallbacks[0].protocol = prov::ProtocolKind::openai_responses;
    const auto outcome = co_await prov::execution::run(backend, request_with_retry(1), std::move(route));
    REQUIRE_FALSE(outcome.response.has_value());
    CHECK(outcome.target.profile == "fallback-profile");
    CHECK(outcome.target.model == "fallback-model");
    CHECK(outcome.target.protocol == prov::ProtocolKind::openai_responses);
    CHECK(outcome.target.fallback);
    CHECK(outcome.response.error().kind() ==
          (failure == Failure::cancellation ? core::ErrorKind::cancelled : core::ErrorKind::internal));
    CHECK_FALSE(outcome.response.error().message().contains("private"));
  });
}
