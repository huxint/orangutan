#pragma once

#include <functional>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>

namespace orangutan::hook {

/// Host callbacks subscribed to events; `Bus` owns the value.
///
/// `observe` receives advisory events. Its failures are isolated from the
/// publisher and from sibling sinks. `decide` gates blocking events; an empty
/// callback proceeds, and an error or exception becomes a `hook_error` veto.
/// Untrusted sinks receive redacted payloads; trusted-local sinks may inspect
/// original inputs, structured tool output and memory text.
///
/// Callbacks run on the publisher's strand. Advisory sinks may run
/// concurrently with sibling sinks, but a sink receives one callback per
/// publish. The shared payload stays alive across suspension points.
struct Sink {
  using Observe = std::function<async::Awaitable<void>(Event, PayloadPtr)>;
  using Decide = std::function<async::Awaitable<core::Result<HookDecision>>(Event, PayloadPtr)>;

  std::string id;
  Observe observe{};
  Decide decide{};
  bool trusted_local{false};
};

}  // namespace orangutan::hook
