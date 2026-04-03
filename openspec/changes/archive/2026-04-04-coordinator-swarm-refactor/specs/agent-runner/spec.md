## ADDED Requirements

### Requirement: Thread-pool-based agent execution
The system SHALL execute spawned agents in a fixed-size thread pool using `std::jthread`. The default pool size SHALL be 4, configurable via `"max_concurrent_agents"` in config. Each agent run SHALL receive its own `std::stop_token` for cooperative cancellation.

#### Scenario: Agent runs in thread pool
- **WHEN** `agent_spawn` is called
- **THEN** the agent task SHALL be submitted to the thread pool
- **THEN** the agent SHALL execute in one of the pool's worker threads

#### Scenario: Thread pool at capacity
- **WHEN** all thread pool workers are busy and a new `agent_spawn` is called
- **THEN** the spawn request SHALL be queued until a worker becomes available
- **THEN** the tool SHALL return immediately with `status: "queued"`

### Requirement: Agent execution context isolation
Each spawned agent SHALL run with its own isolated context: a fresh `ToolRegistry`, its own `Provider` instance (with model from agent definition), its own session for conversation persistence, and its own `ToolRuntimeContext` with `is_child_run = true` to prevent recursive spawning.

#### Scenario: Agent gets isolated tool registry
- **WHEN** an agent is spawned
- **THEN** it SHALL have its own `ToolRegistry` populated according to its agent definition's `tools`/`disallowed_tools`
- **THEN** modifications to the agent's tool state SHALL not affect other agents

#### Scenario: Child agent cannot spawn sub-agents
- **WHEN** a spawned agent attempts to use `agent_spawn`
- **THEN** the tool SHALL not be registered (since `is_child_run = true`)

### Requirement: Agent prompt loop with mailbox integration
Each spawned agent SHALL run an `AgentLoop` that, between LLM turns, polls its mailbox for incoming messages. When messages are found, they SHALL be injected as user-role messages in the agent's conversation before the next LLM call.

#### Scenario: Agent receives message mid-execution
- **WHEN** an agent is running and a message arrives in its mailbox
- **THEN** on the next mailbox poll (between LLM turns), the message SHALL be injected into the conversation
- **THEN** the agent SHALL process the message in its next response

#### Scenario: Agent completes without messages
- **WHEN** an agent runs to completion without receiving any mailbox messages
- **THEN** the agent SHALL complete normally, and its result SHALL be reported to the coordinator

### Requirement: Agent lifecycle management
The `CoordinatorManager` SHALL track all active agent runs with their status: `queued`, `running`, `succeeded`, `failed`, `terminated`, `abandoned`. On shutdown, all active runs SHALL be requested to stop via `stop_token`, given a 5-second grace period, and then marked as `abandoned`.

#### Scenario: Graceful shutdown
- **WHEN** the application receives SIGINT/SIGTERM
- **THEN** `CoordinatorManager::shutdown()` SHALL trigger all agents' stop tokens
- **THEN** agents SHALL be given 5 seconds to finish current work
- **THEN** remaining active runs SHALL be marked as `abandoned`
- **THEN** all thread pool threads SHALL be joined

#### Scenario: Agent run status tracking
- **WHEN** an agent transitions from `running` to `succeeded`
- **THEN** the run record in SQLite SHALL be updated with the final status, completion timestamp, and output summary
- **THEN** if a coordinator is waiting, a `<task-notification>` SHALL be generated

### Requirement: CoordinatorManager replaces SubagentManager
The `CoordinatorManager` class SHALL replace `SubagentManager` as the central orchestration component. It SHALL be created during bootstrap and injected into `ToolRuntimeContext`. The `ToolRuntimeContext` fields `subagent_manager`, `allowed_child_agents`, and `is_child_run` SHALL be replaced with `coordinator_manager`, `team_agents`, and `is_child_run`.

#### Scenario: Bootstrap creates CoordinatorManager
- **WHEN** the application starts
- **THEN** bootstrap SHALL create a `CoordinatorManager` with the agent definition registry, session store, memory store, provider factory, and thread pool configuration
- **THEN** the manager SHALL be injected into all runtime contexts that need agent orchestration

#### Scenario: ToolRuntimeContext carries new fields
- **WHEN** a runtime is built for an agent
- **THEN** `ToolRuntimeContext` SHALL have `coordinator_manager` (pointer), `team_agents` (vector of allowed agent keys), and `is_child_run` (bool)
