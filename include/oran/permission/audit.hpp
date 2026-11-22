#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

/// Durable outcome of one dispatch decision. The first three values mirror
/// `Verdict`; the rest record what approval or a blocking hook did to it.
enum class AuditOutcome : std::uint8_t {
  allow,
  deny,
  ask,
  approved,
  rejected,
  /// A blocking hook vetoed the call before permission evaluation.
  blocked_by_hook,
  /// A blocking hook rewrote the input and the rewritten call was allowed.
  rewritten,
};

/// One audit row. `StorageAuditSink` encodes enums and the input hash into
/// `storage::AppendAuditEventRequest`.
struct AuditEvent {
  /// `permission_decision` for dispatch, `cancellation_lag` for a tool that
  /// outlived batch cancellation.
  std::string event_kind{"permission_decision"};
  /// Required by `StorageAuditSink`; see secrets-and-state "Identity And Scope".
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  /// Verdict from evaluation, kept apart from `outcome` so an approved row
  /// still shows whether a rule or the mode default asked.
  Verdict verdict{Verdict::deny};
  AuditOutcome outcome{AuditOutcome::deny};
  std::string reason;
  /// SHA-256 of the input; absent for rows not tied to one call input.
  std::optional<std::array<std::byte, 32>> input_hash{};
  /// Joins to `trace_turns.turn_id` when the loop correlates traces.
  std::optional<core::TurnId> parent_turn_id{};
  std::string metadata_json{"{}"};
};

class AuditSink {
public:
  AuditSink() = default;
  virtual ~AuditSink() = default;

  AuditSink(const AuditSink&) = delete;
  AuditSink& operator=(const AuditSink&) = delete;
  AuditSink(AuditSink&&) = delete;
  AuditSink& operator=(AuditSink&&) = delete;

  /// Resolves once `event` is as durable as the sink promises: committed for
  /// `StorageAuditSink`, appended in memory for `RecordingAuditSink`.
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> record(AuditEvent event) = 0;
};

/// Discards every event; used when auditing is disabled.
class NullAuditSink final : public AuditSink {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;
};

/// Keeps every event in memory, in insertion order.
class RecordingAuditSink final : public AuditSink {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;

  [[nodiscard]] std::span<const AuditEvent> events() const noexcept {
    return events_;
  }
  void clear() noexcept {
    events_.clear();
  }

private:
  std::vector<AuditEvent> events_;
};

/// Maps a verdict to its outcome before approval changes it.
[[nodiscard]] constexpr AuditOutcome verdict_to_outcome(Verdict verdict) noexcept {
  return core::parse_enum<AuditOutcome>(core::enum_name(verdict)).value_or(AuditOutcome::deny);
}

/// Fills verdict, outcome and reason; callers supply the remaining fields.
[[nodiscard]] AuditEvent make_audit_event_from_decision(const Decision& decision);

/// Encodes a SHA-256 digest as the lowercase hex stored in audit rows.
[[nodiscard]] std::string to_hex(std::span<const std::byte, 32> input_hash);

}  // namespace orangutan::permission

template <>
struct std::formatter<orangutan::permission::AuditOutcome> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::AuditOutcome o, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(o), ctx);
  }
};
