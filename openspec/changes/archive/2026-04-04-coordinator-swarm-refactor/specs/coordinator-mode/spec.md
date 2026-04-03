## ADDED Requirements

### Requirement: Coordinator mode activation via config
The system SHALL support a `coordinator_mode` boolean field on `AgentConfig`. When `coordinator_mode` is `true`, the agent's tool set SHALL be restricted to orchestration-only tools: `agent_spawn`, `agent_send_message`, `agent_stop`. All other tools (bash, file edit, file read, grep, glob, etc.) SHALL be unavailable to the coordinator agent.

#### Scenario: Agent configured as coordinator
- **WHEN** an agent has `"coordinator_mode": true` in config
- **THEN** the agent's tool registry SHALL contain only `agent_spawn`, `agent_send_message`, and `agent_stop`

#### Scenario: Agent not configured as coordinator
- **WHEN** an agent has `"coordinator_mode": false` or the field is absent
- **THEN** the agent's tool registry SHALL contain the full standard tool set

### Requirement: Coordinator system prompt injection
The system SHALL inject a specialized coordinator system prompt when coordinator mode is active. The prompt SHALL define the agent as an "orchestrator" that delegates all implementation work to worker agents. The prompt SHALL describe a 4-phase workflow: Research, Synthesis, Implementation, Verification.

#### Scenario: Coordinator prompt replaces standard guidance
- **WHEN** coordinator mode is active for an agent
- **THEN** the system prompt SHALL include coordinator-specific guidance instead of standard subagent guidance
- **THEN** the prompt SHALL instruct the LLM to never perform direct file operations

### Requirement: Worker agent spawning via agent_spawn tool
The coordinator SHALL spawn worker agents using the `agent_spawn` tool. The tool SHALL accept parameters: `agent_key` (which agent definition to use), `prompt` (the task description), and optional `name` (a human-readable label). The tool SHALL return a `run_id` for tracking.

#### Scenario: Successful worker spawn
- **WHEN** the coordinator calls `agent_spawn` with a valid `agent_key` and `prompt`
- **THEN** a new worker agent SHALL be started asynchronously in the thread pool
- **THEN** the tool SHALL return `{"run_id": "<id>", "status": "running"}`

#### Scenario: Spawn with invalid agent key
- **WHEN** the coordinator calls `agent_spawn` with an `agent_key` not in its `team_agents` list
- **THEN** the tool SHALL return an error: `"agent '<key>' is not in the allowed team_agents list"`

### Requirement: Task notification on worker completion
When a worker agent completes (success or failure), the system SHALL inject a `<task-notification>` XML block into the coordinator's conversation as a user-role message. The notification SHALL include: `task-id`, `status` (completed/failed), `summary`, `result`, and token usage.

#### Scenario: Worker completes successfully
- **WHEN** a worker agent finishes its task without errors
- **THEN** a `<task-notification>` with `status="completed"` SHALL be injected into the coordinator's conversation
- **THEN** the coordinator SHALL be able to read the result and take further action

#### Scenario: Worker fails
- **WHEN** a worker agent encounters an error or times out
- **THEN** a `<task-notification>` with `status="failed"` SHALL be injected with the error details

### Requirement: Worker continuation via agent_send_message
The coordinator SHALL be able to send follow-up messages to running or idle workers using `agent_send_message`. This allows the coordinator to refine tasks, ask clarifying questions, or provide additional context to a worker.

#### Scenario: Send message to running worker
- **WHEN** the coordinator calls `agent_send_message` with a valid `run_id` and `text`
- **THEN** the message SHALL be delivered to the worker's mailbox
- **THEN** the worker SHALL process the message on its next mailbox poll

### Requirement: Worker termination via agent_stop
The coordinator SHALL be able to terminate a worker agent using the `agent_stop` tool with a `run_id`. The worker's `stop_token` SHALL be triggered, and the worker SHALL be given a grace period to clean up before being marked as terminated.

#### Scenario: Stop a running worker
- **WHEN** the coordinator calls `agent_stop` with a valid `run_id`
- **THEN** the worker's cancellation token SHALL be triggered
- **THEN** the worker SHALL be marked as `terminated` after cleanup or timeout
