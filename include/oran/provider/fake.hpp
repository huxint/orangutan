#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider {

/// A piece of a streaming response. Each variant alternative names exactly
/// one `EventSink` callback so a fixture authored as `[TextDelta, ToolStart,
/// ToolInputDelta, StreamEnd]` doubles as a sink-call expectation list.
struct TextDelta {
  std::string text;

  friend bool operator==(const TextDelta&, const TextDelta&) = default;
};

struct ThinkingDelta {
  std::string text;

  friend bool operator==(const ThinkingDelta&, const ThinkingDelta&) = default;
};

struct ToolStart {
  std::string id;
  std::string name;

  friend bool operator==(const ToolStart&, const ToolStart&) = default;
};

struct ToolInputDelta {
  std::string id;
  std::string input_delta;

  friend bool operator==(const ToolInputDelta&, const ToolInputDelta&) = default;
};

/// Terminal delta. `stop_reason` becomes the returned `Response::stop_reason`
/// and the argument to `EventSink::on_done`. Optional `usage` overrides
/// whatever counters the assembler accumulated (the fake does not invent
/// token counts on its own).
struct StreamEnd {
  core::StopReason stop_reason{core::StopReason::end_turn};
  std::optional<Usage> usage;
  std::optional<std::string> model_used;

  friend bool operator==(const StreamEnd&, const StreamEnd&) = default;
};

using StreamDelta = std::variant<TextDelta, ThinkingDelta, ToolStart, ToolInputDelta, StreamEnd>;

/// One scripted assistant turn. At most one of `response`, `deltas`, or
/// `error` carries a body; precedence is `error` > `response` > `deltas` so
/// the test author can leave the others empty. `latency` is awaited before
/// any body is produced and is cancel-aware via `async::sleep_for`.
///
/// A turn with no body at all is a fixture bug; `FakeProvider::send` returns
/// `core::Error::internal("scripted turn has no body")` so the test failure
/// names the bad slot.
struct ScriptedTurn {
  std::optional<Response> response;
  std::vector<StreamDelta> deltas;
  std::optional<core::Error> error;
  std::chrono::milliseconds latency{0};
};

/// Scripted provider. Each `send` call consumes the next `ScriptedTurn` from
/// the plan. Exhausting the plan returns `core::Error::internal` so tests
/// notice when the loop ran longer than the fixture authored.
///
/// Thread-safety. Concurrent `send` calls on one fake are serialised
/// internally; the public surface stays `const` so the fake satisfies the
/// `System` shape without callers reaching for a non-const reference.
class FakeProvider final : public System {
public:
  explicit FakeProvider(std::vector<ScriptedTurn> plan);
  ~FakeProvider() override;

  FakeProvider(const FakeProvider&) = delete;
  FakeProvider& operator=(const FakeProvider&) = delete;
  FakeProvider(FakeProvider&&) = delete;
  FakeProvider& operator=(FakeProvider&&) = delete;

  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send(Request request, ModelTarget target, EventSink* sink = nullptr) const override;

  /// Number of `send` calls that consumed a scripted turn (including failed
  /// turns and exhausted-plan failures).
  [[nodiscard]] std::size_t turns_consumed() const noexcept;

  /// Total scripted turns originally supplied. Constant for the lifetime of
  /// the fake.
  [[nodiscard]] std::size_t plan_size() const noexcept;

  /// True when `turns_consumed() >= plan_size()`. The next `send` call will
  /// return `Error::internal("provider plan exhausted")`.
  [[nodiscard]] bool exhausted() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::provider
