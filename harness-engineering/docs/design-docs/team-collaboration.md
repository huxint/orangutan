# Team Collaboration

Multi-agent orchestration is what turns a single "smart chatbot" into a small workshop.
The legacy `orangutan/` had `OrchestrationManager` but coordination was implicit — the
leader sent prompts, workers polled mailboxes, and termination was up to the leader's
prompt. v2 makes coordination **a strategy object**, the conversation **a DAG**, and
team-shared state **a memory tier**.

## Definitions

| Term            | Meaning                                                            |
| --------------- | ------------------------------------------------------------------ |
| **Agent**       | A `Loop` driven by a base model + tools + identity (see `agent-platform.md`). |
| **Team**        | A set of agents + a coordination strategy + a shared memory scope. |
| **Leader**      | An agent in a team with privileges to spawn / stop teammates and to drive the team's overall direction. |
| **Worker**      | An agent in a team that is not the leader.                          |
| **Mailbox**     | A bounded `async::Channel<MailboxMessage>` per agent.              |
| **Scratchpad** | The team-shared memory tier; see `memory-system.md`.                |
| **Conversation DAG** | The directed graph of who spawned whom and who messaged whom. |

A team is **not** the same as a session. One session may host one or many teams.

## Coordination Strategies

The strategy decides:

- Whose turn it is to act.
- When the team is "done".
- Whether outputs are merged, voted on, or pipelined.

```cpp
// include/oran/orchestration/strategy.hpp — PUBLIC
namespace orangutan::orchestration {

class Strategy {
 public:
  virtual ~Strategy() = default;

  // Called when a teammate produces output; the strategy decides what to do.
  virtual async::Awaitable<Action>
  on_output(const Conversation&, const TeammateOutput&) = 0;

  // Called when a teammate's mailbox receives a message — strategy can re-route.
  virtual async::Awaitable<Action>
  on_message(const Conversation&, const MailboxMessage&) = 0;

  // Termination: strategy declares the team is finished and yields a final summary.
  virtual async::Awaitable<TerminationDecision>
  is_done(const Conversation&) = 0;
};

}  // namespace orangutan::orchestration
```

`Action` is one of: `dispatch_to(agent)`, `broadcast`, `wait_for_human`, `terminate`.

### Built-in Strategies

| Strategy                | Behavior                                                       |
| ----------------------- | -------------------------------------------------------------- |
| `LeaderWorker`          | The leader runs first, spawns/instructs workers, merges their outputs, decides termination. The legacy default. |
| `Pipeline`              | A list of agents; output of agent `i` is the input of `i+1`; terminates at the last. |
| `RoundRobin`            | Each agent takes a turn; iterations capped; terminates on convergence or budget. |
| `Vote`                  | All agents answer the same question independently; an aggregator (configurable: majority, weighted, leader-pick) chooses. |
| `FreeForm`              | Mailbox-based; any agent may message any agent; terminates on explicit "I'm done" message from the leader. |

Custom strategies are registered with `StrategyRegistry` at bootstrap. Adding a
strategy is a small library (`oran-orchestration-strategy-<name>`) with one class.

## Conversation DAG

```cpp
struct Conversation {
  std::string         id;                 // unique per team activation
  std::string         team_id;
  std::vector<Node>   nodes;              // agents, with state
  std::vector<Edge>   edges;              // spawns + messages, in arrival order
  core::Time          started_at;
  std::optional<core::Time> ended_at;
};

struct Node {
  std::string agent_key;
  std::string instance_id;                // unique within this conversation
  Role        role;                       // leader | worker | observer
  Status      status;                     // queued | running | idle | stopped | failed
  std::size_t total_iterations  = 0;
  std::size_t total_tool_calls  = 0;
};

struct Edge {
  std::string from;                       // instance_id (or "team")
  std::string to;                         // instance_id (or "team")
  EdgeKind    kind;                       // spawn, message, broadcast, abort
  core::Time  at;
  std::optional<MessageId> message_id;
};
```

The DAG is **persisted** in `orchestration.db`. Post-hoc:

- The web UI renders it.
- The audit log references nodes/edges.
- The `team-debrief` skill summarizes it.

## Mailbox

Bounded, typed, asio-aware:

```cpp
class Mailbox {
 public:
  Mailbox(async::Runtime&, std::size_t capacity);

  // Sender side.
  async::Awaitable<core::Result<void>> send(MailboxMessage);
  core::Result<void>                    try_send(MailboxMessage);

  // Receiver side (the teammate runtime).
  async::Awaitable<core::Result<MailboxMessage>> receive();

  // Lifecycle.
  void close();
};

struct MailboxMessage {
  std::string  conversation_id;
  std::string  from_instance_id;
  std::string  to_instance_id;            // "team" for broadcast
  std::string  text;
  std::vector<core::Content> attachments;
  std::optional<std::string> idempotency_tag;
  core::Time   sent_at;
  Origin       origin;
};
```

Delivery: at-least-once with **idempotency on `idempotency_tag`**. The teammate runtime
keeps a small ring buffer of seen tags to discard duplicates.

Overflow: `try_send` returns `Error::MailboxOverflowed`. The default policy is
"reject and surface to leader"; an alternate policy "drop-oldest-and-warn-via-hook" is
configurable.

## Teammate Runtime

```cpp
class TeammateRuntime {
 public:
  virtual ~TeammateRuntime() = default;

  // Run a single prompt; may produce output that the strategy consumes.
  virtual async::Awaitable<core::Result<TeammateOutput>>
  run(std::string prompt, core::CancellationSlot) = 0;

  // Whether this teammate accepts follow-up prompts mid-task.
  virtual bool can_receive_followups() const noexcept = 0;

  // Polled when the strategy wants to pass a follow-up.
  virtual async::Awaitable<core::Result<std::optional<MailboxMessage>>>
  poll_mailbox() = 0;
};
```

The default `TeammateRuntime` wraps an `agent::Loop`; it is constructed by the same
`RuntimeAssembler` used for primary CLI runtimes. Alternative `TeammateRuntime`
implementations can:

- Wrap a remote agent on another machine (federated).
- Wrap a non-LLM tool ("workers" that are just shell scripts).
- Mock for testing.

## Shared Scratchpad

A team has a shared memory scope (see `memory-system.md`). Conventions:

- The leader posts a plan to the scratchpad before spawning workers.
- Workers write findings as they go.
- The leader's prompt builder injects a "team-scratchpad" section into each iteration.

The scratchpad is **append-mostly**. Removals require `Capability::write_memory` on the
team scope; default is leader-only.

## Permission Model

A teammate's permission set is the *intersection* of:

- The agent's own config-defined permissions.
- The team's permission overlay (defined in team config).

So a team can be more restrictive than its members but never more permissive. This
prevents a research team from accidentally running shell commands via a coder member.

## Spawning A Team — End-To-End

```text
1. CLI / web / channel invokes orchestration tool: agent.spawn_team(team_def, prompt).
2. oran-orchestration::Manager:
     a. Validates team_def against the team registry (config + builtins).
     b. Creates a Conversation, persists it.
     c. Resolves Strategy.
     d. Constructs TeammateRuntime instances for each member.
     e. Sends `prompt` to the leader via mailbox.
3. The Strategy.on_output / on_message callbacks drive subsequent dispatch.
4. When Strategy.is_done returns Terminate, the leader's final output is returned to
   the caller, the conversation is closed, and a `conversation.completed` hook fires.
```

## Cancellation Semantics

- A leader can `agent.stop(instance_id)` a worker. The worker's loop receives
  cancellation; in-flight tools cancel; the worker's mailbox closes.
- The CLI / web can stop a whole conversation: all member loops cancel; the
  conversation enters `terminated` state; `conversation.aborted` hook fires.
- Cancellation must complete within `config.orchestration.cancel_grace_seconds`
  (default 5s) or the manager logs a warning and force-closes the mailbox.

## Hook Surface

Orchestration events (see `permissions-and-hooks.md`):

- `team.created(team_def, conversation_id)`
- `worker.spawned(team_id, agent_key, instance_id)`
- `worker.stopped(team_id, instance_id, reason)`
- `team.message(team_id, message)` — every mailbox send
- `team.broadcast(team_id, message)`
- `conversation.completed(team_id, conversation_id, summary)`
- `conversation.aborted(team_id, conversation_id, reason)`

## Anti-Patterns

- Workers that spawn workers. Allowed but require a recursion budget
  (`config.orchestration.max_depth`, default 2). A worker that needs another worker
  almost always wants the leader to spawn it instead.
- Strategies that mutate global state. Strategy is stateless (or has only its own
  per-conversation state); manager owns the conversation.
- Mailboxes used as RPC. Mailbox is one-way. For request/response between agents, use
  a small `Conversation`-scoped RPC abstraction (planned post-v1; tracked in the tech
  debt tracker).
- A leader prompt that says "you decide when to stop" without strategy enforcement —
  the strategy must declare termination, not the prompt.

## Bench

`bench/oran-orchestration/` ships:

- `bench/team-spawn` — time from `spawn_team` to leader's first iteration.
- `bench/mailbox-throughput` — sustained message rate before bounded queue saturates.
- `bench/strategy-leader-worker-vs-vote` — A/B between strategies on the same workload.

## See Also

- [`agent-platform.md`](agent-platform.md) for the higher-level vision.
- [`memory-system.md#shared-memory-team`](memory-system.md) for the scratchpad tier.
- [`permissions-and-hooks.md`](permissions-and-hooks.md) for hook contracts.
- [`../product-specs/0004-agent-team.md`](../product-specs/0004-agent-team.md) for v1
  deliverables.
