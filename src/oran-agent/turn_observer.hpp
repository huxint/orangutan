#pragma once

#include <cstdint>
#include <string_view>

#include <oran/agent/loop.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/provider/execution.hpp>

namespace orangutan::agent::detail {

[[nodiscard]] std::int64_t now_epoch_ns() noexcept;

/// Accumulated state that terminal trace records describe.
struct TurnProgress {
  const provider::Usage& usage;
  std::uint32_t iterations;
  const provider::execution::Attribution& target;
};

/// Hook publication and trace records for one turn. Observation never changes
/// the turn's outcome except to annotate an error with a failed trace write.
class TurnObserver {
public:
  TurnObserver(const RunTurnInputs& inputs,
               const provider::Route& route,
               const prompt::RenderedPrompt& rendered,
               std::int64_t started_at_ns) noexcept
      : inputs_{inputs}, route_{route}, rendered_{rendered}, started_at_ns_{started_at_ns} {}

  [[nodiscard]] static bool trace_configured(const RunTurnInputs& inputs) noexcept;

  [[nodiscard]] async::Awaitable<void>
  provider_request(const provider::Request& request, std::uint32_t iteration, core::Time started_at) const;
  [[nodiscard]] async::Awaitable<void> provider_response(const provider::Response& response,
                                                         const provider::execution::Attribution& target,
                                                         std::uint32_t iteration,
                                                         core::Time started_at,
                                                         core::Time finished_at) const;
  [[nodiscard]] async::Awaitable<void> provider_error(const core::Error& error,
                                                      const provider::execution::Attribution& target,
                                                      std::uint32_t iteration,
                                                      core::Time started_at,
                                                      core::Time finished_at) const;

  /// Record a completed turn.
  [[nodiscard]] async::Awaitable<core::Result<void>> complete(core::StopReason stop_reason,
                                                              TurnProgress progress) const;
  /// Record a failed turn and return its error. A cancelled turn is recorded
  /// with `cancellation_phase` after cancellation is shielded for cleanup.
  [[nodiscard]] async::Awaitable<core::Error>
  fail(core::Error error, std::string_view cancellation_phase, TurnProgress progress) const;

private:
  const RunTurnInputs& inputs_;
  const provider::Route& route_;
  const prompt::RenderedPrompt& rendered_;
  std::int64_t started_at_ns_;
};

}  // namespace orangutan::agent::detail
