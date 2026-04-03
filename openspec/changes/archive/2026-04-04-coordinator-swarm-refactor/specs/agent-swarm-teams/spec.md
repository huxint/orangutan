## ADDED Requirements

### Requirement: Team creation via team_create tool
The system SHALL provide a `team_create` tool that creates a named team. A team is a logical group of agents that can communicate and collaborate. Team state SHALL be persisted to SQLite under the workspace directory.

#### Scenario: Create a new team
- **WHEN** `team_create` is called with `name` and optional `description`
- **THEN** a team record SHALL be created in the database with a unique ID, the given name, creation timestamp, and the calling agent registered as team lead
- **THEN** the tool SHALL return `{"team_id": "<id>", "name": "<name>"}`

#### Scenario: Create team with duplicate name
- **WHEN** `team_create` is called with a name that already exists and is active
- **THEN** the tool SHALL return an error indicating the team name is already in use

### Requirement: Team member registration
When an agent is spawned as part of a team (via `agent_spawn` with a `team` parameter), it SHALL be automatically registered as a team member. Each member has an `agent_id`, `name`, `agent_key`, `model`, and `joined_at` timestamp.

#### Scenario: Agent joins team on spawn
- **WHEN** `agent_spawn` is called with a `team` parameter specifying an existing team name
- **THEN** the spawned agent SHALL be registered as a member of that team
- **THEN** the agent SHALL be able to communicate with other team members via mailbox

### Requirement: Team deletion via team_delete tool
The system SHALL provide a `team_delete` tool that tears down a team. All team members SHALL receive a shutdown signal, and the team record SHALL be marked as deleted after all members have stopped.

#### Scenario: Delete an active team
- **WHEN** `team_delete` is called with a valid `team_id`
- **THEN** all active team members SHALL receive a shutdown request via mailbox
- **THEN** members SHALL be given a grace period to finish current work
- **THEN** the team record SHALL be marked as deleted

#### Scenario: Delete a non-existent team
- **WHEN** `team_delete` is called with an invalid `team_id`
- **THEN** the tool SHALL return an error: `"team not found"`

### Requirement: Team state persistence
Team configuration (members, status, metadata) SHALL be persisted to SQLite. The system SHALL recover team state on restart and mark all previously-active members as abandoned.

#### Scenario: Team state survives restart
- **WHEN** the application restarts with active teams in the database
- **THEN** team records SHALL be loaded from SQLite
- **THEN** previously-active members SHALL be marked as `abandoned`
- **THEN** the team lead can choose to respawn abandoned members

### Requirement: Team listing and status
The system SHALL provide a way to query active teams and their members. This SHALL be available via the admin API and optionally as an LLM tool.

#### Scenario: List active teams via admin API
- **WHEN** a GET request is made to the team listing endpoint
- **THEN** the response SHALL include all active teams with their member lists, statuses, and creation timestamps
