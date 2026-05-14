# Memory System

Orangutan v2 makes memory **tiered and explicit**. Each tier has its own backend trait,
its own retention policy, and its own hookable lifecycle. The legacy `orangutan/`
"memory" was a single SQLite store + a MEMORY.md mirror with no concept of tiers; v2
turns that into four distinct concerns.

## Tiers

| Tier        | Lifetime           | Scope                              | Backing                  |
| ----------- | ------------------ | ---------------------------------- | ------------------------ |
| **Working** | one ReAct turn     | this agent, this prompt            | in-process               |
| **Session** | one session        | this agent + this `session_id`     | `sessions.db` (SQLite)   |
| **Long-term** | persistent       | this agent (scope_key)             | `memory.db` (SQLite + FTS5 + optional vector) |
| **Shared**  | persistent         | this team (`team_id`)              | `memory.db`, separate table |

Hooks fire on writes/reads at each tier (see "Hook Surface" below).

## Working Memory

Built up while a single ReAct turn executes. It is the place where:

- The pre-rendered memory section lives (so the prompt is built once per turn).
- Tool outputs accumulate before they are summarized.
- The current iteration's plan / scratchpad / partial answer sits.

Backed by in-process state, owned by `agent::Loop`. Cleared at turn end.
No SQLite involvement.

API (no public stand-alone trait — it's a private member of `agent::Loop`):

```cpp
class Loop {
  struct Working {
    std::string rendered_memory_section;       // computed before the loop
    std::vector<core::Content> scratchpad;     // mutated as the loop runs
    std::optional<std::string> plan;           // if the agent emitted one
  };
};
```

## Session Memory

Conversation history. The legacy `SessionStore` is preserved in spirit: per-session,
per-agent JSON-serialized message stream in SQLite. v2 changes:

- **Expected-only API.** `core::Result<...>` everywhere. No `must_ok` wrappers.
- **One DB connection per writer**, pool for readers; WAL on.
- **Prepared-statement cache** (legacy didn't have one).
- **Schema migrations** versioned and applied at startup.
- **Append-only fast path**: appending a message is one INSERT; loading is one SELECT.

```cpp
// include/oran/memory/session.hpp
namespace orangutan::memory::session {

struct SessionId  { std::string value; };
struct AgentKey   { std::string value; };

class Store {
 public:
  explicit Store(storage::Pool&);

  async::Awaitable<core::Result<void>>
  append(SessionId, AgentKey, core::Message);

  async::Awaitable<core::Result<std::vector<core::Message>>>
  load(SessionId, AgentKey);

  async::Awaitable<core::Result<void>>
  truncate(SessionId, AgentKey, std::size_t keep_last);

  async::Awaitable<core::Result<std::vector<SessionId>>>
  list(AgentKey, ListOpts);
};

}  // namespace orangutan::memory::session
```

## Long-Term Memory

Per-agent persistent facts (about the user, the project, ongoing tasks, learned
patterns). The legacy implementation: SQLite + FTS5 + a single mutex + an optional
MEMORY.md mirror. v2 keeps that core and adds:

- **Connection pool** for reads; one writer connection on a strand.
- **Vector backend slot** (interface, optional): `LongTerm::Backend` is a trait;
  default is the FTS5 backend; an optional `vector_backend` can be plugged in (sqlite-vec,
  HNSW, etc.).
- **Typed kinds** match the legacy ones (user, feedback, project, reference) and gain a
  fifth: `team` (mirrors of shared-tier records for cross-tier search convenience).
- **Decay policy**: `memory-age` style decay is actually wired into the search pipeline
  this time; expired records receive lower BM25 weight before potentially being pruned.

```cpp
// include/oran/memory/longterm.hpp
namespace orangutan::memory::longterm {

struct Record {
  std::string id;
  std::string scope_key;        // agent identity scope
  std::string kind;             // user, feedback, project, reference, team
  std::string title;
  std::string body;
  core::Time   created_at;
  core::Time   updated_at;
  core::Time   last_read_at;
  double       importance = 0.0; // 0..1
  std::vector<std::string> tags;
  std::vector<std::string> linked;  // ids of [[linked]] records
};

class Runtime {
 public:
  Runtime(Backend&, retention::Policy, hook::Bus&);

  async::Awaitable<core::Result<Record>>          get(std::string_view id) const;
  async::Awaitable<core::Result<std::vector<Record>>>
                                                  search(Query, std::size_t limit) const;
  async::Awaitable<core::Result<Record>>          write(WriteRequest) const;
  async::Awaitable<core::Result<void>>            forget(std::string_view id) const;
  async::Awaitable<core::Result<DecaySummary>>    run_decay() const;
};

}  // namespace orangutan::memory::longterm
```

`Backend` is the seam where alternative implementations plug in:

```cpp
class Backend {
 public:
  virtual ~Backend() = default;
  virtual async::Awaitable<core::Result<Record>>             get(std::string_view id) = 0;
  virtual async::Awaitable<core::Result<std::vector<Record>>> search(Query, std::size_t)= 0;
  virtual async::Awaitable<core::Result<Record>>             upsert(Record)            = 0;
  virtual async::Awaitable<core::Result<void>>               remove(std::string_view id)= 0;
};
```

Built-in backends:

- `Fts5Backend` — SQLite FTS5; default.
- `VectorBackend` — optional; uses sqlite-vec or an external embedding store
  via `oran-http::Client` (configurable). Search hybrid: FTS5 score + cosine.

`bench/oran-memory/` (see `docs/product-specs/0010-benchmark-harness.md`) compares
backends on a synthetic 10k-record corpus.

## Shared Memory (Team)

Cross-agent notes for a team. Like long-term but namespaced by `team_id` not
`scope_key`. Use cases:

- Leader posts a high-level plan that workers consult.
- A worker records "I've checked X, Y, Z" so siblings don't redo work.
- Shared findings during a research session.

Same API shape as `longterm::Runtime`, separate `Store` and storage table. Permission
gating: only agents that are members of the team can read/write.

## Reading Memory Once Per Turn

A specific lesson from the legacy code: memory queries inside the ReAct loop were
re-issued every iteration. The prompt section did not change across iterations of a
single turn (the user prompt was fixed) but the code recomputed it anyway.

v2 enforces:

```cpp
async::Awaitable<core::Result<RunResult>>
agent::Loop::run(std::string prompt) {
  // Pre-loop: render memory section ONCE.
  auto memory_section = co_await prompt_builder.render_memory(prompt, identity_);
  working_.rendered_memory_section = std::move(memory_section);

  for (std::size_t i = 0; i < max_iterations_; ++i) {
    // Per-iteration: use working_.rendered_memory_section as cached input.
    auto request = prompt_builder.build(working_, /* skills_section_refresh */ true);
    // ...
  }
}
```

Skills section *is* re-rendered each iteration (intentional, so newly activated skills
surface mid-turn). Memory section is not.

## Retention Policy

`retention::Policy` is a config-defined record:

```cpp
struct Policy {
  std::chrono::days   forget_after_unused = std::chrono::days(180);
  double              importance_floor    = 0.0;  // 0..1; below = candidate for prune
  std::size_t         max_records_per_scope = 10000;
  std::chrono::hours  decay_check_interval  = std::chrono::hours(24);
};
```

A periodic job (registered with `oran-automation`) runs decay according to the policy.
Decayed records are not immediately deleted; they enter a "shadow" state where they
are excluded from default search but visible via `memory.recall("...", include_shadow=true)`.

Forgetting is final (DELETE), with an audit row in `audit.db`.

## MEMORY.md Mirror (Optional)

Same idea as the legacy `memory-mirror`: an optional `<workspace>/.orangutan/memory/
MEMORY.md` file kept in sync with the long-term store, for human inspection. v2 makes
this **opt-in per agent**, configured by `agent.<name>.memory.mirror`.

## Hook Surface

Memory lifecycle:

- `memory.read.before(scope, kind, query)` — may rewrite the query.
- `memory.read.after(scope, kind, results)` — observability.
- `memory.write.before(scope, record)` — may veto, rewrite, or annotate.
- `memory.write.after(scope, record)`
- `memory.forget(scope, id)`
- `memory.decay(scope, count)` — periodic.

These hooks are why team shared memory works: the orchestration leader can install a
`memory.write.after` hook on the shared tier to mirror notes to a Slack channel, for
example.

## Identity / Scope Derivation

Same scheme as legacy:

- `agent_key` (config-defined; e.g. `coder`, `research`).
- `runtime_key` derived per-process (UUID).
- `scope_key = "<agent_key>:<runtime_key>"` for long-term records *unless* config
  pins `scope_key = "<agent_key>"` (shared across runtimes — useful for stable
  expertise).
- `team_id` derived from team definition.

Identity is opaque outside `oran-bootstrap::Identity`. No one else constructs scope
keys.

## Database Layout

Separate files (the audit identified single-DB contention):

- `<workspace>/.orangutan/sessions.db`
- `<workspace>/.orangutan/memory.db`
- `<workspace>/.orangutan/automation.db`
- `<workspace>/.orangutan/audit.db`

Migrations:

- One `migrations/` dir per DB, numbered `0001-<slug>.sql`, `0002-<slug>.sql`, …
- Applied at startup, recorded in a `schema_versions` table per DB.

## Anti-Goals

- We do **not** try to build a vector DB from scratch. If vector search is wanted,
  pick an existing component (sqlite-vec / external API) and plug it in.
- We do **not** try to make memory cross-runtime by default. Multi-runtime memory
  sharing is an explicit deployment decision (set a stable `scope_key`).
- Memory is **not** a fallback storage for arbitrary state. Anything that is not a
  fact about the world goes in session memory, not long-term.

## See Also

- [`permissions-and-hooks.md`](permissions-and-hooks.md)
- [`secrets-and-state.md`](secrets-and-state.md) — DB file layout, migrations.
- [`../product-specs/0005-memory-system.md`](../product-specs/0005-memory-system.md)
  — concrete v1 deliverables.
