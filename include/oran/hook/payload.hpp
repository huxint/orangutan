#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>

namespace orangutan::hook {

/// Identity columns every event payload duplicates. Captured once on the
/// dispatch context and copied per event so sinks don't have to chase the
/// dispatch context's reference back up the stack.
struct Identity {
  std::string scope_key;
  std::string agent_key;
  std::string identity;  // operator / agent identity
};

/// Pre-dispatch payload. Published just after the registry resolves the
/// tool definition but before permission evaluation, so sinks see every
/// known call attempt regardless of how the call is gated.
struct ToolBeforePayload {
  std::string tool_name;
  std::string input_json;
  /// Optional sanitized input view. When present, `Bus` delivers this value
  /// as `input_json` to sinks that are not `Sink::trusted_local`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  /// Wall-clock instant the dispatch started — sinks correlate
  /// `tool_before` with `tool_after` via this field plus tool_name.
  core::Time started_at{};
};

/// Usage metrics copied from `tool::Output::usage` without making
/// `oran-hook` depend on `oran-tool`.
struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read{};
  std::optional<std::uintmax_t> bytes_written{};
  std::optional<std::uint32_t> files_touched{};
  std::optional<std::uint64_t> match_count{};
  std::optional<double> cost_estimate{};
  std::optional<std::chrono::nanoseconds> wall_time{};
  bool truncated{false};
  bool data_dropped{false};

  friend bool operator==(const ToolUsage&, const ToolUsage&) = default;
};

/// Post-dispatch payload. Published at every exit from `Registry::dispatch`
/// (handler returned, permission denied, broker rejection, audit error).
/// Failure observers filter on `succeeded`; `error_kind` / `error_message`
/// carry the propagated error.
struct ToolAfterPayload {
  std::string tool_name;
  std::string input_json;
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  bool succeeded{false};
  /// Verbatim `Output::text` on success; empty string on failure.
  std::string output_text;
  /// Raw structured output bytes copied from `Output::data_json` on success.
  /// `Bus` redacts this field for sinks that are not `Sink::trusted_local`.
  std::optional<std::string> data_json{};
  /// Metrics copied from `Output::usage` on success; all fields empty on
  /// dispatch failure.
  ToolUsage usage{};
  /// `core::Error::kind` enumerator wire spelling (e.g. `permission_denied`,
  /// `not_found`, `internal`) on failure; empty on success.
  std::string error_kind;
  /// `core::Error::message` on failure; empty on success.
  std::string error_message;
  core::Time started_at{};
  core::Time finished_at{};
  /// `finished_at - started_at`. Stored separately so sinks don't have to
  /// recompute it from two `core::Time` values.
  std::chrono::nanoseconds duration{0};
};

/// Approval prompt payload. Published when a permission rule returns `ask`
/// and the dispatch has an approval broker but no caller-supplied token.
/// UI-facing sinks render this into their own channel-specific prompt and
/// return a blocking decision: `proceed` approves, `veto` denies.
struct PermissionAskRenderedPayload {
  std::string tool_name;
  std::string input_json;
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  /// Human-readable rule/hook reason that caused the ask.
  std::string decision_reason;
  /// Replay policy copied from the matched permission decision.
  std::uint32_t replay_max{8};
  std::chrono::seconds approval_ttl{3600};
  core::Time requested_at{};
  std::optional<core::TurnId> turn_id{};
};

/// Long-term memory record snapshot copied without making `oran-hook` depend
/// on `oran-memory`. The `kind` field is the `RecordKind` wire spelling.
struct MemoryRecordPayload {
  std::string id;
  std::string scope_key;
  std::string kind;
  std::string title;
  std::string body;
  core::Time created_at{};
  core::Time updated_at{};
  core::Time last_read_at{};
  double importance{0.0};
  std::vector<std::string> tags{};
  std::vector<std::string> linked_record_ids{};
  bool shadow{false};
};

/// Redacted long-term memory record snapshot delivered to non-trusted sinks.
/// It preserves routing and sizing metadata while omitting title/body/tags and
/// linked ids, which may carry private user or project facts.
struct RedactedMemoryRecordPayload {
  std::string id;
  std::string scope_key;
  std::string kind;
  std::size_t title_bytes{0};
  std::size_t body_bytes{0};
  std::size_t tag_count{0};
  std::size_t linked_record_count{0};
  bool shadow{false};
};

/// Blocking pre-write memory payload. Published after the memory tool has been
/// parsed and scoped but before the long-term backend is mutated.
struct MemoryWritePayload {
  Identity who;
  MemoryRecordPayload record;
  /// Optional sanitized metadata view. When present, `Bus` clears sensitive
  /// text/list fields from `record` for sinks that are not `Sink::trusted_local`.
  std::optional<RedactedMemoryRecordPayload> redacted_record{};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Recall results are copied at publication to keep hook types independent
/// of `oran-memory`.
struct MemoryReadHitPayload {
  MemoryRecordPayload record;
  double score{0.0};
  /// Optional sanitized metadata view. When present, `Bus` clears sensitive
  /// text/list fields from `record` for sinks that are not `Sink::trusted_local`.
  std::optional<RedactedMemoryRecordPayload> redacted_record{};
};

/// Advisory read memory payload. Published after a successful long-term recall
/// at either the prompt boundary or the `MemoryRecall` tool boundary.
struct MemoryReadPayload {
  Identity who;
  /// MemoryRecall supplies full records. MemoryRecall:index supplies only
  /// discovery cues in hit titles/bodies; other record metadata is unset.
  std::string source;
  /// Raw recall query for trusted-local sinks. If `redacted_query_bytes` is set,
  /// `Bus` clears this field for non-trusted sinks.
  std::string query;
  std::optional<std::size_t> redacted_query_bytes{};
  std::size_t limit{0};
  std::vector<std::string> kinds;
  std::size_t match_count{0};
  std::vector<MemoryReadHitPayload> hits;
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Advisory delete memory payload. Published after a scoped long-term memory
/// delete succeeds.
struct MemoryForgetPayload {
  Identity who;
  std::string id;
  std::string scope_key;
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Provider token/cost counters copied without making `oran-hook` depend on
/// `oran-provider`.
struct ProviderUsage {
  std::uint64_t input_tokens{0};
  std::uint64_t output_tokens{0};
  std::uint64_t cache_creation_tokens{0};
  std::uint64_t cache_read_tokens{0};
  std::optional<double> cost_estimate{};

  friend bool operator==(const ProviderUsage&, const ProviderUsage&) = default;
};

/// Provider request metadata. The payload intentionally carries counts and
/// route identity, not prompt text, message bodies, headers, or credentials.
struct ProviderRequestPayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::size_t fallback_count{0};
  std::size_t message_count{0};
  std::size_t tool_count{0};
  bool stream{true};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  std::uint32_t retry_max_attempts{0};
  std::chrono::milliseconds retry_initial_backoff{0};
  core::Time started_at{};
};

/// Provider response metadata. `served_*` names the concrete route target that
/// produced the response after execution-layer retry/fallback attribution; a
/// fallback is a served profile that differs from `route_profile`.
struct ProviderResponsePayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::string served_profile;
  std::string served_model;
  std::string served_protocol;
  std::string stop_reason;
  ProviderUsage usage{};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Provider error metadata. Advisory sinks see the failure class and message,
/// while raw request/response bodies stay outside the hook payload.
struct ProviderErrorPayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::string error_kind;
  std::string error_message;
  bool retryable{false};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Credential/body-free observation before a channel admission or HTTP effect.
struct ChannelActionPayload {
  std::string platform;
  std::string account;
  std::string operation;
  bool allowed{false};
};

using Payload = std::variant<ChannelActionPayload,
                             ToolBeforePayload,
                             ToolAfterPayload,
                             PermissionAskRenderedPayload,
                             MemoryReadPayload,
                             MemoryWritePayload,
                             MemoryForgetPayload,
                             ProviderRequestPayload,
                             ProviderResponsePayload,
                             ProviderErrorPayload>;

/// Shared immutable payload delivered to sinks. `Bus` builds at most one raw
/// and one redacted snapshot per publish and shares them across sinks.
using PayloadPtr = std::shared_ptr<const Payload>;

}  // namespace orangutan::hook
