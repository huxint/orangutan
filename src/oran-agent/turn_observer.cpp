#include "turn_observer.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <asio/cancellation_state.hpp>
#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/time.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/storage/trace_repository.hpp>

namespace orangutan::agent::detail {
std::int64_t now_epoch_ns() noexcept {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

[[nodiscard]] hook::Identity hook_identity(const RunTurnInputs& inputs) {
  return hook::Identity{
      .scope_key = std::string{inputs.scope_key},
      .agent_key = std::string{inputs.agent_key},
      .identity = std::string{inputs.identity},
  };
}

[[nodiscard]] hook::ProviderUsage hook_usage(const provider::Usage& usage) {
  return hook::ProviderUsage{
      .input_tokens = usage.input_tokens,
      .output_tokens = usage.output_tokens,
      .cache_creation_tokens = usage.cache_creation_tokens,
      .cache_read_tokens = usage.cache_read_tokens,
      .cost_estimate = usage.cost_estimate,
  };
}

[[nodiscard]] std::chrono::nanoseconds duration_between(core::Time start, core::Time finish) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(finish.to_system_time_point() -
                                                              start.to_system_time_point());
}

[[nodiscard]] hook::ProviderRequestPayload make_provider_request_payload(const RunTurnInputs& inputs,
                                                                         const provider::Request& request,
                                                                         const provider::Route& route,
                                                                         std::uint32_t iteration,
                                                                         core::Time started_at) {
  return hook::ProviderRequestPayload{
      .who = hook_identity(inputs),
      .origin = std::string{inputs.origin},
      .turn_id = inputs.turn_id,
      .iteration = iteration,
      .route_profile = route.primary.profile,
      .route_model = route.primary.model,
      .route_protocol = std::string{core::enum_name(route.primary.protocol)},
      .fallback_count = route.fallbacks.size(),
      .message_count = request.messages.size(),
      .tool_count = request.tools.size(),
      .stream = request.stream,
      .max_tokens = request.max_tokens,
      .thinking_budget = request.thinking_budget,
      .retry_max_attempts = request.retry.max_attempts,
      .retry_initial_backoff = request.retry.initial_backoff,
      .started_at = started_at,
  };
}

[[nodiscard]] hook::ProviderResponsePayload
make_provider_response_payload(const RunTurnInputs& inputs,
                               const provider::Response& response,
                               const provider::Route& route,
                               const provider::execution::Attribution& target,
                               std::uint32_t iteration,
                               core::Time started_at,
                               core::Time finished_at) {
  return hook::ProviderResponsePayload{
      .who = hook_identity(inputs),
      .origin = std::string{inputs.origin},
      .turn_id = inputs.turn_id,
      .iteration = iteration,
      .route_profile = route.primary.profile,
      .route_model = route.primary.model,
      .route_protocol = std::string{core::enum_name(route.primary.protocol)},
      .served_profile = target.profile,
      .served_model = target.model,
      .served_protocol = std::string{core::enum_name(target.protocol)},
      .stop_reason = std::string{core::enum_name(response.stop_reason)},
      .usage = hook_usage(response.usage),
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = duration_between(started_at, finished_at),
  };
}

[[nodiscard]] hook::ProviderErrorPayload
make_provider_error_payload(const RunTurnInputs& inputs,
                            const core::Error& error,
                            const provider::execution::Attribution& failing_target,
                            std::uint32_t iteration,
                            core::Time started_at,
                            core::Time finished_at) {
  return hook::ProviderErrorPayload{
      .who = hook_identity(inputs),
      .origin = std::string{inputs.origin},
      .turn_id = inputs.turn_id,
      .iteration = iteration,
      .route_profile = failing_target.profile,
      .route_model = failing_target.model,
      .route_protocol = std::string{core::enum_name(failing_target.protocol)},
      .error_kind = std::string{core::enum_name(error.kind())},
      .error_message = std::string{error.message()},
      .retryable = error.retryable(),
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = duration_between(started_at, finished_at),
  };
}

[[nodiscard]] async::Awaitable<void> publish_provider_request(const RunTurnInputs& inputs,
                                                              const provider::Request& request,
                                                              const provider::Route& route,
                                                              std::uint32_t iteration,
                                                              core::Time started_at) {
  if (inputs.bus == nullptr) {
    co_return;
  }
  co_await inputs.bus->publish_advisory(hook::Event::provider_request,
                                        make_provider_request_payload(inputs, request, route, iteration, started_at));
}

[[nodiscard]] async::Awaitable<void> publish_provider_response(const RunTurnInputs& inputs,
                                                               const provider::Response& response,
                                                               const provider::Route& route,
                                                               const provider::execution::Attribution& target,
                                                               std::uint32_t iteration,
                                                               core::Time started_at,
                                                               core::Time finished_at) {
  if (inputs.bus == nullptr) {
    co_return;
  }
  co_await inputs.bus->publish_advisory(
      hook::Event::provider_response,
      make_provider_response_payload(inputs, response, route, target, iteration, started_at, finished_at));
}

[[nodiscard]] async::Awaitable<void> publish_provider_error(const RunTurnInputs& inputs,
                                                            const core::Error& error,
                                                            const provider::execution::Attribution& failing_target,
                                                            std::uint32_t iteration,
                                                            core::Time started_at,
                                                            core::Time finished_at) {
  if (inputs.bus == nullptr) {
    co_return;
  }
  co_await inputs.bus->publish_advisory(
      hook::Event::provider_error,
      make_provider_error_payload(inputs, error, failing_target, iteration, started_at, finished_at));
}

[[nodiscard]] core::Result<std::int64_t> checked_i64(std::uint64_t value, std::string field) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(
        core::Error::invalid_argument("trace counter exceeds storage range").with("field", std::move(field)));
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] core::Result<std::int64_t> checked_size_i64(std::size_t value, std::string field) {
  if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(
        core::Error::invalid_argument("trace byte count exceeds storage range").with("field", std::move(field)));
  }
  return static_cast<std::int64_t>(value);
}


[[nodiscard]] bool trace_writer_configured(const RunTurnInputs& inputs) noexcept {
  return inputs.trace.enabled && inputs.trace.repository != nullptr;
}

[[nodiscard]] core::Result<storage::AppendTraceTurnRequest>
make_trace_request(const RunTurnInputs& inputs,
                   const provider::Route& route,
                   const prompt::RenderedPrompt& rendered,
                   const provider::Usage& usage,
                   std::uint32_t iterations,
                   std::int64_t started_at_ns,
                   std::string route_model,
                   std::string served_profile,
                   core::StopReason stop_reason,
                   std::optional<std::string> cancellation_phase = std::nullopt) {
  if (inputs.trace.repository == nullptr) {
    return std::unexpected(core::Error::invalid_argument("trace repository is not configured"));
  }
  if (!inputs.trace.enabled) {
    return std::unexpected(core::Error::invalid_argument("trace writer is disabled"));
  }
  if (!inputs.turn_id.has_value()) {
    return std::unexpected(core::Error::invalid_argument("trace writer requires a turn id"));
  }

  if (served_profile.empty()) {
    served_profile = route.primary.profile;
  }

  auto prefix_bytes = checked_size_i64(rendered.prefix_bytes, "prompt_prefix_bytes");
  if (!prefix_bytes) {
    return std::unexpected(prefix_bytes.error());
  }
  auto input_tokens = checked_i64(usage.input_tokens, "input_tokens");
  if (!input_tokens) {
    return std::unexpected(input_tokens.error());
  }
  auto output_tokens = checked_i64(usage.output_tokens, "output_tokens");
  if (!output_tokens) {
    return std::unexpected(output_tokens.error());
  }
  auto cache_creation_tokens = checked_i64(usage.cache_creation_tokens, "cache_creation_tokens");
  if (!cache_creation_tokens) {
    return std::unexpected(cache_creation_tokens.error());
  }
  auto cache_read_tokens = checked_i64(usage.cache_read_tokens, "cache_read_tokens");
  if (!cache_read_tokens) {
    return std::unexpected(cache_read_tokens.error());
  }

  return storage::AppendTraceTurnRequest{
      .turn_id = *inputs.turn_id,
      .parent_turn_id = inputs.trace.parent_turn_id,
      .session_id = inputs.trace.session_id,
      .agent_key = std::string{inputs.trace.agent_key},
      .origin = std::string{inputs.trace.origin},
      .route_profile = std::move(served_profile),
      .route_model = std::move(route_model),
      .started_at_ns = started_at_ns,
      .finished_at_ns = now_epoch_ns(),
      .stop_reason = std::string{core::enum_name(stop_reason)},
      .iteration_count = static_cast<std::int64_t>(iterations),
      .prompt_prefix_hash = rendered.prefix_hash,
      .prompt_prefix_bytes = *prefix_bytes,
      .active_catalog_hash = rendered.tool_catalog_hash,
      .cache_creation_tokens = *cache_creation_tokens,
      .cache_read_tokens = *cache_read_tokens,
      .input_tokens = *input_tokens,
      .output_tokens = *output_tokens,
      .cost_estimate_usd = usage.cost_estimate.value_or(0.0),
      .cancellation_phase = std::move(cancellation_phase),
      .context_json = std::string{inputs.trace.context_json},
  };
}

[[nodiscard]] async::Awaitable<core::Result<void>>
write_trace_turn(const RunTurnInputs& inputs,
                 const provider::Route& route,
                 const prompt::RenderedPrompt& rendered,
                 const provider::Usage& usage,
                 std::uint32_t iterations,
                 std::int64_t started_at_ns,
                 std::string route_model,
                 std::string served_profile,
                 core::StopReason stop_reason,
                 std::optional<std::string> cancellation_phase = std::nullopt) {
  if (!trace_writer_configured(inputs)) {
    co_return core::Result<void>{};
  }
  auto request = make_trace_request(inputs,
                                    route,
                                    rendered,
                                    usage,
                                    iterations,
                                    started_at_ns,
                                    std::move(route_model),
                                    std::move(served_profile),
                                    stop_reason,
                                    std::move(cancellation_phase));
  if (!request) {
    co_return std::unexpected(std::move(request).error());
  }
  try {
    auto appended = co_await asio::co_spawn(inputs.trace.blocking_executor,
                                            inputs.trace.repository->append_turn(std::move(*request)),
                                            asio::use_awaitable);
    if (!appended) {
      co_return std::unexpected(std::move(appended).error());
    }
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return core::Result<void>{};
  } catch (const std::system_error& error) {
    if (error.code() == asio::error::operation_aborted) {
      co_return std::unexpected(core::Error::cancelled());
    }
    co_return std::unexpected(core::Error::storage("trace write failed").with("cause", error.what()));
  } catch (const std::exception& error) {
    co_return std::unexpected(core::Error::storage("trace write failed").with("cause", error.what()));
  }
}

[[nodiscard]] core::Error attach_trace_write_error(core::Error original, core::Error trace_error) {
  return std::move(original).with("trace_write_failed", std::string{trace_error.message()});
}

}  // namespace

bool TurnObserver::trace_configured(const RunTurnInputs& inputs) noexcept {
  return trace_writer_configured(inputs);
}

async::Awaitable<void>
TurnObserver::provider_request(const provider::Request& request, std::uint32_t iteration, core::Time started_at) const {
  co_await publish_provider_request(inputs_, request, route_, iteration, started_at);
}

async::Awaitable<void> TurnObserver::provider_response(const provider::Response& response,
                                                       const provider::execution::Attribution& target,
                                                       std::uint32_t iteration,
                                                       core::Time started_at,
                                                       core::Time finished_at) const {
  co_await publish_provider_response(inputs_, response, route_, target, iteration, started_at, finished_at);
}

async::Awaitable<void> TurnObserver::provider_error(const core::Error& error,
                                                    const provider::execution::Attribution& target,
                                                    std::uint32_t iteration,
                                                    core::Time started_at,
                                                    core::Time finished_at) const {
  co_await publish_provider_error(inputs_, error, target, iteration, started_at, finished_at);
}

async::Awaitable<core::Result<void>> TurnObserver::complete(core::StopReason stop_reason,
                                                            TurnProgress progress) const {
  co_return co_await write_trace_turn(inputs_,
                                      route_,
                                      rendered_,
                                      progress.usage,
                                      progress.iterations,
                                      started_at_ns_,
                                      progress.target.model,
                                      progress.target.profile,
                                      stop_reason);
}

async::Awaitable<core::Error>
TurnObserver::fail(core::Error error, std::string_view cancellation_phase, TurnProgress progress) const {
  if (!trace_writer_configured(inputs_)) {
    co_return error;
  }
  const bool cancelled = error.kind() == core::ErrorKind::cancelled;
  if (cancelled) {
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  }
  auto traced = co_await write_trace_turn(inputs_,
                                          route_,
                                          rendered_,
                                          progress.usage,
                                          progress.iterations,
                                          started_at_ns_,
                                          progress.target.model,
                                          progress.target.profile,
                                          cancelled ? core::StopReason::cancelled : core::StopReason::error,
                                          cancelled ? std::optional<std::string>{cancellation_phase} : std::nullopt);
  if (!traced) {
    error = attach_trace_write_error(std::move(error), std::move(traced).error());
  }
  co_return error;
}

}  // namespace orangutan::agent::detail
