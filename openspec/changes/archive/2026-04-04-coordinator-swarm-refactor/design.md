## Context

The orangutan project currently implements a single-level subagent delegation system where a parent LLM agent spawns child agents in separate threads. Children cannot spawn their own children, there is no inter-agent communication, and the parent must manually poll/block-wait for results. This architecture was modeled after a simple fork-exec pattern.

Claude Code has since shipped two mature multi-agent paradigms: **Coordinator Mode** (pure orchestrator with restricted tools dispatching async workers) and **Agent Swarms/Teams** (persistent named agents communicating via file-based mailboxes). Both are battle-tested and provide the patterns we need.

Key constraints:
- orangutan is a C++ application (C++20/23), not TypeScript -- we adapt the architecture, not port it
- The existing thread-pool-based execution model in SubagentManager is sound; we keep thread-based concurrency rather than Node.js coroutines
- SQLite is already used for sessions; we keep it for team state persistence rather than switching to flat JSON files (better atomicity)
- The existing `ToolRuntimeContext` pattern for injecting dependencies into tools is the right pattern to extend
- Config format changes must be backwards-compatible where possible or clearly documented as breaking

## Goals / Non-Goals

**Goals:**
- Replace the 3-tool subagent API (`subagent_spawn/status/wait`) with a richer coordinator/swarm tool set (`agent_spawn`, `agent_send_message`, `agent_stop`, `team_create`, `team_delete`)
- Enable coordinator mode: a main agent that acts as pure orchestrator with a restricted tool set and specialized system prompt
- Enable inter-agent communication via a mailbox system (direct messages, broadcasts)
- Support declarative agent definitions loaded from config and `.orangutan/agents/` directories
- Provide task notification system for async worker result aggregation
- Maintain the existing single-process, multi-threaded execution model
- Keep the system simple and incremental -- this is a C++ project, not a TypeScript monorepo

**Non-Goals:**
- Multi-process agent execution (tmux/iTerm2 panes) -- we stay in-process with threads
- Permission synchronization between agents (agents share the same process; permissions are inherited)
- Visual pane management / UI for concurrent agents
- Agent worktree isolation (git worktree per agent) -- future enhancement
- Backwards compatibility with old `"subagents"` config field -- this is a clean break

## Decisions

### 1. Single `CoordinatorManager` replaces `SubagentManager`

**Decision:** Replace `SubagentManager` with a single `CoordinatorManager` class that handles both coordinator mode (restricted orchestrator) and team mode (peer-to-peer agent communication).

**Rationale:** Claude Code separates these into distinct code paths (coordinator vs swarm), but they share 80% of the infrastructure (agent spawning, lifecycle, result collection). In C++ we benefit from a single manager that can operate in either mode. The manager holds a thread pool, agent registry, mailbox system, and run store.

**Alternatives considered:**
- Separate `CoordinatorManager` and `SwarmManager` classes: rejected because of duplicated agent lifecycle code
- Keep `SubagentManager` and layer coordinator on top: rejected because the polling-based API is fundamentally wrong for coordinator mode which needs push-based notifications

### 2. File-based mailbox with SQLite backing for durability

**Decision:** Agent mailboxes use JSON inbox files (one per agent) for fast reads, with SQLite write-ahead for crash recovery. Messages are `{from, to, text, timestamp, read, type}` structs.

**Rationale:** Claude Code's pure-file approach works in TypeScript's single-threaded model but is prone to corruption with C++ threads. SQLite gives us atomic writes. We keep JSON files as a cache layer for fast reads during the mailbox polling loop.

**Alternatives considered:**
- Pure SQLite: simpler but slower for the hot polling path
- Pure JSON with file locks: matches Claude Code but risky in multi-threaded C++
- In-memory ring buffer: fastest but loses messages on crash

### 3. Thread pool with `std::jthread` replaces per-run `std::thread`

**Decision:** Use a fixed-size `std::jthread` pool (default 4 workers) instead of spawning a new thread per agent run.

**Rationale:** The current `SubagentManager` creates a `std::thread` per spawn, which doesn't bound concurrency. A thread pool with `std::jthread` (cooperative cancellation via `stop_token`) is cleaner and prevents resource exhaustion when the coordinator spawns many workers.

**Alternatives considered:**
- Keep per-run threads with a semaphore: works but wasteful
- `std::async` with custom executor: no standard executor in C++20

### 4. Agent definitions via config + directory scanning

**Decision:** Agents are defined in two ways: (a) in `config.json` under `"agents"` with expanded fields, (b) as markdown files in `.orangutan/agents/` with YAML frontmatter (description, tools, model, prompt).

**Rationale:** Matches Claude Code's pattern of built-in + custom agent definitions. The config path is for system agents; the directory path is for user-defined agents that can be version-controlled.

### 5. Coordinator system prompt injection via `identity.cpp`

**Decision:** When coordinator mode is active, the existing `append_subagent_prompt_guidance()` is replaced with `append_coordinator_prompt()` that sets the orchestrator role and restricts available tools in the system prompt.

**Rationale:** This follows the existing pattern in `identity.cpp` where runtime identity and prompt guidance are derived. The coordinator prompt defines the 4-phase workflow (Research → Synthesis → Implementation → Verification) and instructs the LLM to use only orchestration tools.

### 6. Task notification via conversation injection

**Decision:** When a worker agent completes, its result is formatted as a `<task-notification>` XML block and injected into the coordinator's conversation as a user-role message, exactly matching Claude Code's pattern.

**Rationale:** This is the proven pattern from Claude Code. The coordinator LLM naturally processes notifications in its conversation flow without needing a separate callback mechanism.

### 7. Config schema evolution

**Decision:** Replace `AgentConfig::subagents` with:
```cpp
struct AgentConfig {
    // ... existing fields ...
    bool coordinator_mode = false;           // enables orchestrator role
    std::vector<std::string> team_agents;    // agents this one can spawn/message
    std::string agent_definition;            // optional: path to .md definition file
};
```

**Rationale:** `coordinator_mode` is a boolean flag (you're either an orchestrator or not). `team_agents` replaces `subagents` with broader semantics (spawn + communicate, not just spawn). `agent_definition` links to the richer markdown definition format.

## Risks / Trade-offs

- **[Breaking config change]** → Document migration guide; the change is mechanical (`"subagents"` → `"team_agents"`, add `"coordinator_mode": true` if desired)
- **[Mailbox polling latency]** → Default poll interval of 100ms; configurable. Acceptable for non-realtime use cases
- **[Thread pool sizing]** → Default 4 workers may be too few for heavy coordinator workloads → Make configurable via `"max_concurrent_agents"` in config
- **[Agent definition loading at startup]** → Scanning `.orangutan/agents/` adds startup time → Cache definitions, lazy-load on first use
- **[Coordinator prompt quality]** → The coordinator system prompt heavily influences behavior → Port and adapt Claude Code's proven prompt template
- **[SQLite contention]** → Multiple agent threads + mailbox writes on same SQLite DB → Use WAL mode (already default) and keep transactions short

## Migration Plan

1. Remove all `src/subagent/` code, `src/storage/subagent-run-store.*`, `src/tools/subagent/`
2. Remove `SubagentRunStore` creation from bootstrap and all references
3. Add new source directories: `src/coordinator/`, `src/swarm/`
4. Update `AgentConfig` in `config.hpp` -- remove `subagents`, add new fields
5. Implement `CoordinatorManager` + `AgentMailbox` + `AgentDefinitionLoader`
6. Implement new tools: `agent_spawn`, `agent_send_message`, `agent_stop`, `team_create`, `team_delete`
7. Update bootstrap wiring to use new manager
8. Update web routes for team/agent APIs
9. Update frontend components
10. Update tests

**Rollback:** Git revert. No data migration needed (subagent run DB is ephemeral state).

## Open Questions

- Should agents in team mode be able to spawn further agents (multi-level delegation)? Current design says no (matches existing restriction), but coordinator workers in Claude Code also cannot spawn sub-workers.
- Should we support hot-reloading of agent definitions from `.orangutan/agents/` or only load at startup?
- What is the right default thread pool size? 4 may be too conservative for coordinator mode with many research workers.
