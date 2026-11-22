#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/team.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool {

class Registry;
class PathLocks;
struct DispatchContext;

struct AgentRunRequest {
  std::string prompt;
  bool background{false};
  std::string label{};
};

struct TaskRequest {
  std::string task_id;
  std::size_t offset{0};
  std::size_t max_bytes{8192};
};
using TaskHandler = std::function<async::Awaitable<core::Result<Output>>(TaskRequest, DispatchContext&)>;
using TeamHandler = std::function<async::Awaitable<core::Result<Output>>(TeamCommand, DispatchContext&)>;

/// The host owns child admission and lifetime; dispatch validates and delivers results.
using AgentRunHandler =
    std::function<async::Awaitable<core::Result<Output>>(AgentRunRequest request, DispatchContext& ctx)>;

struct MemoryRecallRequest {
  std::string query{};
  std::size_t limit{5};
  std::vector<std::string> kinds{};
  /// Exact read; absent query and id select the paginated index.
  std::string id{};
  std::size_t offset{0};

  friend bool operator==(const MemoryRecallRequest&, const MemoryRecallRequest&) = default;
};

struct MemoryRememberRequest {
  std::string id;
  std::string kind;
  std::string title;
  std::string body;
  double importance{0.5};
  std::vector<std::string> tags;
  std::vector<std::string> linked_record_ids;

  friend bool operator==(const MemoryRememberRequest&, const MemoryRememberRequest&) = default;
};

struct MemoryForgetRequest {
  std::string id;

  friend bool operator==(const MemoryForgetRequest&, const MemoryForgetRequest&) = default;
};

/// Runtime callback consumed by the `MemoryRecall` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory runtime through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryRecallHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryRecallRequest request, DispatchContext& ctx)>;

/// Runtime callback consumed by the `MemoryRemember` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory backend through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryRememberHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryRememberRequest request, DispatchContext& ctx)>;

/// Runtime callback consumed by the `MemoryForget` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory backend through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryForgetHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryForgetRequest request, DispatchContext& ctx)>;

/// Pinned filesystem target for a prepared call. Executors use
/// `authority` and `authority_relative_path`; display paths are metadata.
struct ResolvedToolPath {
  std::optional<io::DirectoryAuthority> authority{};
  std::string authority_relative_path;
  std::string absolute_path;
  std::string relative_path;
  /// Stable label used in audit metadata. For ordinary workspace paths this is
  /// `<workspace>/...` or an extra-root label; for per-call outside overrides
  /// this is the resolved absolute path.
  std::string display_path;
  std::string input_path_hash;
  std::string workspace_root_hash;
  bool symlink_followed{false};
  bool created_parents{false};
  /// True for configured extra-root matches and per-call outside read/list
  /// overrides.
  bool outside_workspace_explicit_override{false};
  bool per_call_outside_workspace_override{false};
  std::optional<std::size_t> override_root_index{};
};

/// Per-call context the registry threads into every handler. Holds references
/// to the long-lived permission infrastructure plus the per-call identity
/// strings the audit pipeline needs.
///
/// The caller retains borrowed services until dispatch completes. Use `for_now`
/// to create a fresh per-call snapshot of a reusable prototype.
struct DispatchContext {
  /// Create a fresh context for the current wall clock. This is the
  /// production default for callers that do not need a pinned broker clock;
  /// tests that need deterministic approval TTL behavior can still aggregate
  /// initialise the struct and set `now` explicitly.
  [[nodiscard]] static DispatchContext for_now(asio::any_io_executor executor,
                                               std::span<const permission::Rule> rules,
                                               permission::AuditSink& audit,
                                               std::string scope_key = {},
                                               std::string agent_key = {},
                                               std::string identity = {});

  /// Clone a caller-owned prototype for one dispatch, refreshing `now` and
  /// clearing fields that `Registry::dispatch` mutates. `approval_token_output`
  /// is copied only when the caller can guarantee the output slot is not
  /// shared by concurrent dispatches.
  [[nodiscard]] static DispatchContext for_now(const DispatchContext& prototype, bool thread_approval_token_output);

  asio::any_io_executor executor;
  permission::Mode mode{permission::Mode::default_};
  std::span<const permission::Rule> rules;
  permission::AuditSink& audit;
  /// An inherited policy is evaluated separately on the final input. Its rules
  /// remain alive until this context and every dispatch snapshot finish.
  std::optional<permission::PolicyView> parent_policy{};
  /// Broker for exact-input, identity-bound grants and the blocking approval
  /// hook. Borrowed through dispatch; no consumer or valid grant means refusal.
  permission::ApprovalBroker* approval_broker{nullptr};
  /// Optional approval token. See `approval_broker` above for how
  /// `dispatch` consumes the pair. The pointer is non-owning; the
  /// caller keeps the token alive across the dispatch invocation.
  const permission::ApprovalToken* approval_token{nullptr};
  /// Optional output slot for approvals issued after a blocking
  /// `permission_ask_rendered` prompt. When a sink approves an ask and no
  /// caller-supplied `approval_token` was present, `dispatch` stores the
  /// freshly issued token here before checking it through the broker. The
  /// caller may keep the token for identical-input replay until the broker
  /// exhausts or expires the grant.
  permission::ApprovalToken* approval_token_output{nullptr};
  /// Caller-supplied clock sample for approval expiry. `for_now` samples wall
  /// time; a path-lock wait advances this sample by its monotonic elapsed time
  /// before permission evaluation. Direct callers may pin the initial sample.
  core::Time now{};
  /// Optional hook bus. When non-null, `dispatch` publishes blocking
  /// `hook::Event::tool_before` after the registry resolves the tool def
  /// (i.e., for every known tool name) and consumes veto / rewrite /
  /// require_approval decisions before workspace resolution and permission
  /// evaluation. It publishes advisory `tool_after` at every exit. The
  /// pointer is non-owning; the caller (typically the agent loop) keeps the
  /// bus alive across dispatch invocations.
  hook::Bus* bus{nullptr};
  /// Optional long-term memory recall service. When set, `MemoryRecall` calls
  /// it with the parsed query, limit, and kind spellings, and returns the
  /// produced output through the ordinary dispatch path. When unset,
  /// `MemoryRecall` reports a model-repairable missing-runtime error.
  MemoryRecallHandler memory_recall{};
  /// Optional long-term memory write service. When set, `MemoryRemember` calls
  /// it with parsed record fields and returns the produced output through the
  /// ordinary dispatch path. When unset, `MemoryRemember` reports a
  /// model-repairable missing-runtime error.
  MemoryRememberHandler memory_remember{};
  /// Optional long-term memory delete service. When set, `MemoryForget` calls
  /// it with the parsed record id and returns the produced output through the
  /// ordinary dispatch path. When unset, `MemoryForget` reports a
  /// model-repairable missing-runtime error.
  MemoryForgetHandler memory_forget{};
  /// A bounded child-session runner supplied by the host.
  AgentRunHandler agent_run{};
  TaskHandler task_get{};
  TaskHandler task_cancel{};
  TeamHandler team{};
  /// Optional host-selected tools for a model turn. A direct runtime operation
  /// leaves this absent. Selection limits availability, not external authority.
  std::optional<std::span<const std::string>> active_tools{};
  /// Workspace used to resolve prepared path intent. Borrowed through dispatch;
  /// FileWrite and FileEdit require this authority even for direct callers.
  Workspace* workspace{nullptr};
  /// Shared path exclusion borrowed through dispatch completion. The scheduler
  /// supplies its owned resource; direct callers may share one on their strand.
  PathLocks* path_locks{nullptr};
  /// Pinned path selected by the prepared call after path admission and before
  /// permission evaluation. Cleared on entry so callers can reuse the context.
  std::optional<ResolvedToolPath> resolved_path{};
  /// Output byte caps applied before successful results leave dispatch.
  OutputCapOptions output_caps{};
  /// Optional parent turn id supplied by `agent::Loop` when trace correlation
  /// is enabled. `Registry::dispatch` copies it into `permission::AuditEvent`
  /// so tool audit rows can join to `trace_turns.turn_id`.
  std::optional<core::TurnId> parent_turn_id{};
  /// Per-process scope key the audit row gets stamped with. See
  /// `docs/design-docs/secrets-and-state.md` "Identity And Scope".
  std::string scope_key;
  /// Agent making the call. Defaults to empty so unit tests that don't care
  /// about the agent identifier can stay terse.
  std::string agent_key;
  /// Operator / agent identity bound to the call. Carried into the audit row
  /// and (eventually) into the approval broker.
  std::string identity;
};

/// Ordinary handlers receive final hook input and validate their own arguments.
/// Use a preparer when validation or path intent must precede admission.
using Handler =
    std::function<async::Awaitable<core::Result<Output>>(std::string_view input_json, DispatchContext& ctx)>;

enum class PathIntent {
  none,
  read,
  write,
};

/// Path intent contains no authority. `none` requests only capability-based
/// exclusion for an ordinary handler that owns its filesystem authorization.
struct PathRequest {
  std::string path;
  PathIntent intent{PathIntent::none};
  WriteIntent write_intent{};
  bool allow_outside_workspace{false};
};

/// One validated call. Capture owned arguments in `execute`; dispatch retains
/// this value through execution and cleanup, and invokes it at most once.
struct PreparedCall {
  std::optional<PathRequest> path;
  std::move_only_function<async::Awaitable<core::Result<Output>>(DispatchContext&)> execute;
};

/// Pure preparation from final hook input; no filesystem access or other effects.
using Preparer = std::function<core::Result<PreparedCall>(std::string_view input_json)>;

/// Runtime tools manage host-bound state; external tools require permission.
/// Only trusted registration selects this policy, never model input.
enum class DispatchPolicy {
  permissioned,
  runtime
};

class Registry {
public:
  Registry() = default;

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry(Registry&&) noexcept = default;
  Registry& operator=(Registry&&) noexcept = default;
  ~Registry() = default;

  /// Register `def` + `handler`. Returns `Error::conflict` if a tool with
  /// the same name is already registered, `Error::invalid_argument` if
  /// either piece is malformed (empty name, invalid `input_schema_json`, or
  /// null handler).
  [[nodiscard]] core::Result<void> add(core::ToolDef def, Handler handler);

  /// Register argument preparation before admission and approval. The prepared
  /// executor shares audit and output handling. Permissioned is the default;
  /// trusted internal bindings opt into runtime scope/admission instead.
  [[nodiscard]] core::Result<void>
  add_prepared(core::ToolDef def, Preparer prepare, DispatchPolicy policy = DispatchPolicy::permissioned);

  /// Remove the tool named `name`. Returns `Error::not_found` if no such
  /// tool was registered.
  [[nodiscard]] core::Result<void> remove(std::string_view name);

  /// Number of registered tools.
  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

  /// Lookup a tool by name. Returns a pointer that is valid until the next
  /// `add` / `remove`; null when no tool matches.
  [[nodiscard]] const core::ToolDef* find(std::string_view name) const;

  /// All registered tool definitions, ordered by insertion. Useful for the
  /// agent-loop's "advertise this catalog to the provider" step.
  [[nodiscard]] std::vector<core::ToolDef> catalog() const;

  /// Run one tool through hooks, path admission, authorization and execution.
  /// `tool_before` finalizes input once. Preparation validates arguments before
  /// acquiring any path lock, resolving pinned authority, evaluating both policies
  /// and checking exact-input approval. A durable audit decision precedes the
  /// handler; veto, failed admission, denial or audit failure prevents effects.
  /// Output caps and completion observations finish before the lock is released.
  /// Unknown names return `not_found` without publishing hooks. Callers retain
  /// the context and its borrowed services until dispatch completes.
  /// See docs/design-docs/tool-runtime.md for ordering and failure contracts.
  [[nodiscard]] async::Awaitable<core::Result<Output>>
  dispatch(std::string_view name, std::string_view input_json, DispatchContext& ctx) const;

private:
  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }

    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
      return (*this)(std::string_view{value});
    }
  };

  struct Entry {
    core::ToolDef def;
    Preparer prepare;
    DispatchPolicy policy{DispatchPolicy::permissioned};
    std::size_t insertion_index{0};
  };

  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries_;
  std::size_t next_index_{0};
};

}  // namespace orangutan::tool
