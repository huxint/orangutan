#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/agent/context.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/core/working_context.hpp>
#include <oran/prompt/render.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::tool {
class Registry;
struct DispatchContext;
}  // namespace orangutan::tool

namespace orangutan::storage {
class TraceRepository;
}  // namespace orangutan::storage

namespace orangutan::hook {
class Bus;
}  // namespace orangutan::hook

namespace orangutan::agent {

class ToolScheduler;

struct LoopOptions {
  std::uint32_t max_iterations{16};

  friend bool operator==(const LoopOptions&, const LoopOptions&) = default;
};

struct TraceContext {
  /// Disabling tracing also clears turn correlation on tool audit rows.
  bool enabled{true};
  /// Borrowed until the turn's terminal write completes, including cancellation.
  /// A configured repository requires a blocking executor and session identity.
  storage::TraceRepository* repository{nullptr};
  asio::any_io_executor blocking_executor{};
  core::TurnId session_id{};
  std::optional<core::TurnId> parent_turn_id{};
  std::string_view agent_key{};
  std::string_view origin{};
  std::string_view context_json{"{}"};
};

struct RunTurnInputs {
  /// Stable section (1). An empty value selects `agent::default_system_preamble()`.
  /// Supplying text is an explicit override for tests or embedders that already
  /// own a preamble. Stable text is copied once per turn,
  /// before the first provider call; the next turn observes host edits.
  std::string_view system_preamble{};
  /// Available definitions. The selected native catalogue is sorted once per
  /// turn and shared by prompt fingerprinting and every provider request.
  std::span<const core::ToolDef> tool_catalog{};
  /// Absent selects all available tools; an explicit empty list selects none.
  std::optional<std::span<const std::string>> active_tools{};
  std::string_view skills_catalog{};
  std::string_view memory_framing{};
  std::string_view per_agent_overlay{};
  /// Conversation tail, already including the current user turn. The loop copies
  /// these messages into the provider request; the span only needs to remain
  /// valid until the coroutine is awaited to completion.
  std::span<const core::Message> conversation_tail{};
  ContextOptions context{};
  core::WorkingContext checkpoint{};
  std::int64_t history_end{};
  /// Forward reader over a fixed persisted snapshot, starting after sequence.
  /// Returns a bounded nonempty page until history_end is reached.
  std::function<async::Awaitable<core::Result<std::vector<core::Message>>>(std::int64_t)> history_loader{};
  /// Bounded untrusted data before each model request. Empty means no messages;
  /// the host acknowledges delivery only after transcript persistence.
  std::function<core::Result<std::string>()> inbox_reader{};
  std::optional<std::string> tool_choice{};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  bool stream{true};
  /// Threads through tool audits when tracing is enabled. A configured trace
  /// writer generates this id before rendering if the caller omits it.
  std::optional<core::TurnId> turn_id{};
  TraceContext trace{};
  /// Optional process hook bus. When supplied, the loop publishes advisory
  /// provider lifecycle events around every provider await: request, response,
  /// error, and fallback attribution. Payloads carry route/usage metadata only;
  /// prompt bytes and provider bodies stay out of hook delivery.
  hook::Bus* bus{nullptr};
  std::string_view scope_key{};
  std::string_view agent_key{};
  std::string_view identity{};
  std::string_view origin{};
  /// Both are required to execute tool-use responses through authorized dispatch.
  tool::Registry* tools{nullptr};
  tool::DispatchContext* dispatch_context{nullptr};
  /// Optional parallel tool-call scheduler. When set, the loop
  /// routes every tool batch — including N == 1 — through
  /// `ToolScheduler::run_batch`, so bounded parallelism, per-path locks,
  /// per-call timeout, and parent-cancellation propagation apply uniformly.
  /// `bootstrap::AgentSession` owns one for the runner's lifetime. When
  /// null but `tools`/`dispatch_context` are present, the loop builds a
  /// per-turn scheduler with default options so embedders and tests still get
  /// the single batched dispatch path.
  ToolScheduler* scheduler{nullptr};
};

struct RunTurnResult {
  /// Terminal assistant text; `assistant_blocks` retains the typed response.
  std::string text;
  std::vector<core::Content> assistant_blocks;
  core::StopReason stop_reason{core::StopReason::end_turn};
  provider::Usage usage{};
  std::string model_used{};
  /// Owned system text and cache identity shared by this turn's iterations.
  prompt::RenderedPrompt rendered_prompt{};
  std::uint32_t iterations{0};
  /// Original conversation_tail plus new responses/results; excludes loaded
  /// history and synthetic summaries. Session callers supply only the new user
  /// request and persist this suffix atomically with the checkpoint.
  std::vector<core::Message> transcript;
  core::WorkingContext checkpoint;
};

class Loop {
public:
  Loop(provider::System& provider, provider::Route route, LoopOptions options = {});
  ~Loop();

  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;
  Loop(Loop&&) noexcept;
  Loop& operator=(Loop&&) noexcept;

  /// Run a provider/tool turn through the supplied dispatch services and bounded
  /// scheduler. Borrowed context and trace services must outlive the turn and
  /// its tool cleanup. Cancellation during provider or tool work is surfaced as
  /// `ErrorKind::cancelled` with `reason=parent_cancelled` plus
  /// `cancellation_phase=provider_initial|provider_stream|provider_complete|tools|context`;
  /// when a trace context is configured, the same phase is persisted before
  /// the cancelled result is returned.
  /// When `LoopOptions::max_iterations` is exhausted by repeated tool_use
  /// responses and a trace context is configured, an `error` row is written
  /// with the turn's prefix identity and the cumulative usage
  /// before `Error::internal` (reason=`iteration_cap`) returns.
  [[nodiscard]] async::Awaitable<core::Result<RunTurnResult>> run_turn(RunTurnInputs inputs,
                                                                       provider::EventSink* sink = nullptr);

  [[nodiscard]] const provider::Route& route() const noexcept;
  [[nodiscard]] const LoopOptions& options() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent
