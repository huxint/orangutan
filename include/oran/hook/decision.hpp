#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orangutan::hook {

/// Decision a blocking sink returns to the producer's effect gate.
enum class HookDecisionKind : std::uint8_t {
  /// Default. Allow the dispatch pipeline to continue with the original
  /// input. Sinks that observe-only should leave their decision in this
  /// state.
  proceed,
  /// Treat the dispatch as `permission_denied`. The handler is not run.
  /// The sink's `reason` is recorded on the audit row.
  veto,
  /// Substitute `rewritten_input_json` for the original input before
  /// permission evaluation runs. The handler receives the rewritten
  /// input. The audit row records both `input_hash` and
  /// `rewritten_input_hash`.
  rewrite,
  /// Route the dispatch through the approval broker even when the
  /// permission engine would otherwise have returned `allow`.
  require_approval,
};

/// One sink decision observed during a blocking publish. The bus fills this
/// trace in subscription order so dispatch can serialize it into audit
/// metadata without each sink knowing about the audit layer.
struct HookDecisionTrace {
  std::string sink_id;
  HookDecisionKind kind{HookDecisionKind::proceed};
  std::string reason;
  /// Populated when the bus synthesized the decision from a timed-out
  /// blocking sink. Advisory and ordinary blocking decisions leave it empty.
  std::optional<std::chrono::milliseconds> elapsed{};

  friend bool operator==(const HookDecisionTrace&, const HookDecisionTrace&) = default;
};

/// Returned by every blocking publish. The `kind` carries the
/// short-circuit semantics; `reason` is free-form text that travels to
/// the audit row; `rewritten_input_json` is required when `kind ==
/// rewrite` and ignored otherwise.
///
/// The bus initialises every decision with `kind = proceed` and an empty
/// `reason`. A sink that does nothing (or has no blocking handler bound)
/// therefore yields a proceed decision and the next subscribed sink runs.
struct HookDecision {
  HookDecisionKind kind{HookDecisionKind::proceed};
  std::string reason{};
  /// Serialised JSON bytes — same envelope as
  /// `ToolBeforePayload::input_json`. Stored as a string so the public
  /// header does not need to drag in `<nlohmann/json.hpp>`.
  std::optional<std::string> rewritten_input_json{};
  /// Per-sink decisions the bus actually evaluated, in subscription order.
  /// If the first sink vetoes, the trace contains one row; if all sinks
  /// proceed, it contains every sink with a `decide` callback.
  std::vector<HookDecisionTrace> trace{};
};

}  // namespace orangutan::hook
