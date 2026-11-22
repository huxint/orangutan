#include <oran/provider/execution.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::provider::execution {
namespace {

class AttemptSink final : public EventSink {
public:
  explicit AttemptSink(EventSink* inner) noexcept : inner_{inner} {}

  [[nodiscard]] bool emitted() const noexcept {
    return emitted_;
  }

  void on_text_delta(std::string_view delta) override {
    emitted_ = true;
    inner_->on_text_delta(delta);
  }

  void on_thinking_delta(std::string_view delta) override {
    emitted_ = true;
    inner_->on_thinking_delta(delta);
  }

  void on_tool_start(std::string_view id, std::string_view name) override {
    emitted_ = true;
    inner_->on_tool_start(id, name);
  }

  void on_tool_delta(std::string_view id, std::string_view input_delta) override {
    emitted_ = true;
    inner_->on_tool_delta(id, input_delta);
  }

  void on_done(core::StopReason stop_reason) override {
    emitted_ = true;
    inner_->on_done(stop_reason);
  }

private:
  EventSink* inner_;
  bool emitted_{false};
};

[[nodiscard]] core::Error
with_target_context(core::Error error, const Attribution& target, std::uint32_t attempt, std::uint32_t max_attempts) {
  return std::move(error)
      .with("provider_profile", target.profile)
      .with("provider_model", target.model)
      .with("protocol", std::string{core::enum_name(target.protocol)})
      .with("route_role", target.fallback ? "fallback" : "primary")
      .with("attempt", std::to_string(attempt))
      .with("max_attempts", std::to_string(max_attempts));
}

void apply_profile_cost_if_missing(Usage& usage, const ProviderPricing& pricing) {
  if (usage.cost_estimate.has_value() || pricing.empty()) {
    return;
  }
  const auto input = pricing.input_per_million_usd.value_or(0.0);
  const auto output = pricing.output_per_million_usd.value_or(input);
  const auto creation = pricing.cache_creation_per_million_usd.value_or(input);
  const auto read = pricing.cache_read_per_million_usd.value_or(input);
  usage.cost_estimate =
      (static_cast<double>(usage.input_tokens) * input + static_cast<double>(usage.output_tokens) * output +
       static_cast<double>(usage.cache_creation_tokens) * creation +
       static_cast<double>(usage.cache_read_tokens) * read) /
      1'000'000.0;
}

// Fallback policy replaces the primary's thinking override when specified.
void apply_fallback_thinking_policy(Request& request, const ModelTarget& target) noexcept {
  if (target.thinking_budget.has_value()) {
    request.thinking_budget = target.thinking_budget;
  } else if (target.protocol != ProtocolKind::anthropic_messages) {
    request.thinking_budget = std::nullopt;
  }
}

}  // namespace

async::Awaitable<Outcome> run(const System& backend, Request request, Route route, EventSink* sink) {
  Outcome outcome{
      .target = {route.primary.profile, route.primary.model, route.primary.protocol},
      .response = Response{},
  };
  const auto max_attempts = request.retry.max_attempts;
  std::uint32_t attempt = 0;
  try {
    if (max_attempts == 0) {
      outcome.response =
          std::unexpected(core::Error::invalid_argument("provider retry policy requires at least one attempt")
                              .with("field", "retry.max_attempts"));
      co_return outcome;
    }

    for (std::size_t target_index = 0; target_index <= route.fallbacks.size(); ++target_index) {
      const auto& target = target_index == 0 ? route.primary : route.fallbacks[target_index - 1];
      outcome.target = {target.profile, target.model, target.protocol, target_index != 0};
      for (std::uint32_t index = 0; index < max_attempts; ++index) {
        attempt = index + 1;
        auto attempt_request = request;
        if (outcome.target.fallback) {
          apply_fallback_thinking_policy(attempt_request, target);
        }
        AttemptSink attempt_sink{sink};
        auto* effective_sink = sink == nullptr ? nullptr : &attempt_sink;
        outcome.response = co_await backend.send(std::move(attempt_request), target, effective_sink);
        if (outcome.response) {
          auto& response = *outcome.response;
          apply_profile_cost_if_missing(response.usage, target.pricing);
          if (response.model_used && !response.model_used->empty()) {
            outcome.target.model = *response.model_used;
          }
          co_return outcome;
        }

        auto error = with_target_context(std::move(outcome.response).error(), outcome.target, attempt, max_attempts);
        const bool terminal =
            error.kind() == core::ErrorKind::cancelled || !error.retryable() || attempt_sink.emitted();
        if (attempt_sink.emitted() && error.retryable()) {
          error.with("retry_skipped", "stream_already_emitted").with("fallback_skipped", "stream_already_emitted");
        }
        outcome.response = std::unexpected(std::move(error));
        if (terminal) {
          co_return outcome;
        }
        if (attempt == max_attempts) {
          break;
        }

        const auto delay =
            std::ranges::max(outcome.response.error().retry_after().value_or(request.retry.initial_backoff),
                             request.retry.initial_backoff);
        if (delay.count() > 0) {
          auto executor = co_await asio::this_coro::executor;
          auto slept = co_await async::sleep_for(executor, delay);
          if (!slept) {
            outcome.response =
                std::unexpected(with_target_context(std::move(slept).error(), outcome.target, attempt, max_attempts));
            co_return outcome;
          }
        }
      }
    }
  } catch (const std::system_error& error) {
    outcome.response =
        std::unexpected(with_target_context(error.code() == asio::error::operation_aborted
                                                ? core::Error::cancelled()
                                                : core::Error::internal("provider execution failed unexpectedly"),
                                            outcome.target,
                                            attempt,
                                            max_attempts));
  } catch (...) {
    outcome.response =
        std::unexpected(with_target_context(core::Error::internal("provider execution failed unexpectedly"),
                                            outcome.target,
                                            attempt,
                                            max_attempts));
  }
  co_return outcome;
}

}  // namespace orangutan::provider::execution
