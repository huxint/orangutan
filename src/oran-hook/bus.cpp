#include <oran/hook/bus.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <exception>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/sleep.hpp>
#include <oran/async/task_group.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {
namespace {

using namespace std::chrono_literals;
using SinkPtr = std::shared_ptr<const Sink>;

struct SinkDecision {
  HookDecision decision;
  std::optional<std::chrono::milliseconds> elapsed{};
};

void clear_memory_text(MemoryRecordPayload& record) {
  record.title.clear();
  record.body.clear();
  record.tags.clear();
  record.linked_record_ids.clear();
}

[[nodiscard]] Payload redact_payload(Payload payload) {
  std::visit(
      [](auto& alt) {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (requires {
                        alt.input_json;
                        alt.redacted_input_json;
                      }) {
          if (alt.redacted_input_json.has_value()) {
            alt.input_json = *alt.redacted_input_json;
          }
        }
        if constexpr (std::same_as<T, ToolAfterPayload>) {
          alt.data_json.reset();
        }
        if constexpr (std::same_as<T, MemoryWritePayload>) {
          if (alt.redacted_record.has_value()) {
            clear_memory_text(alt.record);
          }
        }
        if constexpr (std::same_as<T, MemoryReadPayload>) {
          if (alt.redacted_query_bytes.has_value()) {
            alt.query.clear();
          }
          for (auto& hit : alt.hits) {
            if (hit.redacted_record.has_value()) {
              clear_memory_text(hit.record);
            }
          }
        }
      },
      payload);
  return payload;
}

/// One raw and one redacted snapshot, each built only when a sink needs it.
struct SharedPayloads {
  PayloadPtr trusted;
  PayloadPtr redacted;

  [[nodiscard]] const PayloadPtr& for_sink(const Sink& sink) const noexcept {
    return sink.trusted_local ? trusted : redacted;
  }
};

[[nodiscard]] SharedPayloads make_shared_payloads(std::span<const SinkPtr> sinks, Payload payload) {
  const bool needs_trusted = std::ranges::any_of(sinks, &Sink::trusted_local);
  const bool needs_redacted = !std::ranges::all_of(sinks, &Sink::trusted_local);

  SharedPayloads shared;
  if (needs_trusted) {
    shared.trusted = std::make_shared<Payload>(std::move(payload));
    if (needs_redacted) {
      shared.redacted = std::make_shared<Payload>(redact_payload(*shared.trusted));
    }
  } else if (needs_redacted) {
    shared.redacted = std::make_shared<Payload>(redact_payload(std::move(payload)));
  }
  return shared;
}

[[nodiscard]] HookDecision hook_error_veto(std::string_view cause, std::string_view sink_id) {
  auto reason = std::string{"hook_error"};
  if (!cause.empty()) {
    reason.append(": ").append(cause);
  }
  reason.append(" [sink=").append(sink_id).append("]");
  return HookDecision{.kind = HookDecisionKind::veto, .reason = std::move(reason)};
}

[[nodiscard]] async::Awaitable<SinkDecision> call_decide(SinkPtr sink, Event event, PayloadPtr payload) {
  try {
    auto result = co_await sink->decide(event, std::move(payload));
    if (!result) {
      co_return SinkDecision{.decision = hook_error_veto(result.error().message(), sink->id)};
    }
    co_return SinkDecision{.decision = std::move(result).value()};
  } catch (const std::exception& ex) {
    co_return SinkDecision{.decision = hook_error_veto(ex.what(), sink->id)};
  } catch (...) {
    co_return SinkDecision{.decision = hook_error_veto("sink threw non-std exception", sink->id)};
  }
}

[[nodiscard]] async::Awaitable<core::Result<void>> sleep_for_timeout(std::chrono::milliseconds timeout) {
  const auto executor = co_await asio::this_coro::executor;
  co_return co_await async::sleep_for(executor, timeout);
}

[[nodiscard]] async::Awaitable<core::Result<SinkDecision>>
call_decide_within(SinkPtr sink, Event event, PayloadPtr payload, std::chrono::milliseconds timeout) {
  if (timeout <= 0ms) {
    co_return co_await call_decide(std::move(sink), event, std::move(payload));
  }

  using namespace asio::experimental::awaitable_operators;
  auto raced = co_await (call_decide(std::move(sink), event, std::move(payload)) || sleep_for_timeout(timeout));
  if (auto* decision = std::get_if<SinkDecision>(&raced); decision != nullptr) {
    co_return std::move(*decision);
  }
  if (auto timer = std::get<core::Result<void>>(std::move(raced)); !timer) {
    co_return std::unexpected(std::move(timer).error());
  }
  co_return SinkDecision{
      .decision = HookDecision{.kind = HookDecisionKind::veto, .reason = "hook_timeout"},
      .elapsed = timeout,
  };
}

[[nodiscard]] async::Awaitable<core::Result<void>> call_observe(SinkPtr sink, Event event, PayloadPtr payload) {
  try {
    co_await sink->observe(event, std::move(payload));
  } catch (...) {
    // Observers cannot affect the publisher or sibling sinks.
  }
  co_return core::Result<void>{};
}

}  // namespace

Bus::Bus(BusOptions options) : options_(options) {}

void Bus::subscribe(Sink sink, std::initializer_list<Event> events) {
  const auto owned = std::make_shared<const Sink>(std::move(sink));
  for (const auto event : events) {
    bindings_[event].push_back(owned);
  }
}

async::Awaitable<void> Bus::publish_advisory(Event event, Payload payload) {
  const auto it = bindings_.find(event);
  if (it == bindings_.end()) {
    co_return;
  }
  auto observers = std::vector<SinkPtr>{};
  std::ranges::copy_if(it->second, std::back_inserter(observers), [](const SinkPtr& sink) {
    return static_cast<bool>(sink->observe);
  });
  if (observers.empty()) {
    co_return;
  }

  auto tasks = async::TaskGroup::create(
      co_await asio::this_coro::executor,
      async::TaskGroupOptions{.max_tasks = observers.size(), .max_completed = observers.size()});
  if (!tasks) {
    co_return;
  }
  const auto payloads = make_shared_payloads(observers, std::move(payload));
  for (auto& sink : observers) {
    auto sink_payload = payloads.for_sink(*sink);
    static_cast<void>(tasks->spawn("hook-" + sink->id, [sink, event, sink_payload]() mutable {
      return call_observe(std::move(sink), event, std::move(sink_payload));
    }));
  }
  if (options_.advisory_timeout > 0ms) {
    static_cast<void>(co_await tasks->join_with_timeout(options_.advisory_timeout));
  } else {
    static_cast<void>(co_await tasks->join());
  }
}

async::Awaitable<core::Result<HookDecision>> Bus::publish_blocking_impl(Event event, Payload payload) {
  const auto it = bindings_.find(event);
  if (it == bindings_.end()) {
    co_return HookDecision{};
  }
  // Copy the subscribers so a subscription made while a sink is suspended
  // cannot invalidate this walk.
  const auto sinks = it->second;
  const auto payloads = make_shared_payloads(sinks, std::move(payload));
  const auto timeout = event == Event::permission_ask_rendered ? 0ms : options_.blocking_timeout;
  auto trace = std::vector<HookDecisionTrace>{};
  for (const auto& sink : sinks) {
    if (!sink->decide) {
      continue;
    }
    auto consulted = co_await call_decide_within(sink, event, payloads.for_sink(*sink), timeout);
    if (!consulted) {
      co_return std::unexpected(std::move(consulted).error());
    }
    auto decision = std::move(consulted->decision);
    trace.push_back(HookDecisionTrace{
        .sink_id = sink->id,
        .kind = decision.kind,
        .reason = decision.reason,
        .elapsed = consulted->elapsed,
    });
    if (decision.kind != HookDecisionKind::proceed) {
      decision.trace = std::move(trace);
      co_return decision;
    }
  }
  co_return HookDecision{.trace = std::move(trace)};
}

}  // namespace orangutan::hook
