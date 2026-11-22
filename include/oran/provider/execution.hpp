#pragma once

#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider::execution {

/// Execution-owned identity, available on both success and failure.
struct Attribution {
  std::string profile;
  std::string model;
  ProtocolKind protocol{ProtocolKind::anthropic_messages};
  bool fallback{false};

  friend bool operator==(const Attribution&, const Attribution&) = default;
};

struct Outcome {
  Attribution target;
  core::Result<Response> response;
};

/// Run bounded attempts and fallbacks over a borrowed backend. Visible stream
/// output prevents replay. The terminal result includes target attribution and
/// successful usage includes a profile estimate when the endpoint omits cost.
[[nodiscard]] async::Awaitable<Outcome>
run(const System& backend, Request request, Route route, EventSink* sink = nullptr);

}  // namespace orangutan::provider::execution
