#pragma once

#include <chrono>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

struct BusOptions {
  /// Bound for each extension gate decision. Approval prompts
  /// (`permission_ask_rendered`) wait for a human decision or cancellation.
  /// Zero disables the bound.
  std::chrono::milliseconds blocking_timeout{2000};
  /// Bound for the advisory fan-out join. A sink that ignores cancellation is
  /// abandoned at the deadline; zero joins unconditionally.
  std::chrono::milliseconds advisory_timeout{2000};

  friend bool operator==(const BusOptions&, const BusOptions&) = default;
};

class Bus {
public:
  explicit Bus(BusOptions options = {});

  /// Take ownership of `sink` and subscribe it to `events`, after previously
  /// subscribed sinks.
  void subscribe(Sink sink, std::initializer_list<Event> events);

  /// Deliver an observation to every subscribed `observe` callback as
  /// concurrent children and join them. Failures and exceptions are isolated;
  /// sinks abandoned at `advisory_timeout` keep their shared ownership until
  /// they finish.
  [[nodiscard]] async::Awaitable<void> publish_advisory(Event event, Payload payload);

  /// Ask subscribed `decide` callbacks in subscription order. The first
  /// non-`proceed` decision wins; errors, exceptions and timeouts veto with a
  /// `hook_error`/`hook_timeout` reason. The trace lists consulted sinks.
  template <Event E>
    requires(is_gate(E))
  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> publish_blocking(Payload payload) {
    return publish_blocking_impl(E, std::move(payload));
  }

private:
  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> publish_blocking_impl(Event event, Payload payload);

  std::unordered_map<Event, std::vector<std::shared_ptr<const Sink>>> bindings_;
  BusOptions options_{};
};

}  // namespace orangutan::hook
