## ADDED Requirements

### Requirement: Per-agent inbox
Each agent in a team SHALL have a dedicated inbox. The inbox SHALL support receiving messages from other agents (direct) or from broadcasts. Messages SHALL be stored in order and marked as read/unread.

#### Scenario: Agent receives a direct message
- **WHEN** another agent sends a message with `to: "<agent-name>"`
- **THEN** the message SHALL appear in the target agent's inbox
- **THEN** the message SHALL have `read: false` initially

#### Scenario: Agent receives a broadcast message
- **WHEN** another agent sends a message with `to: "*"`
- **THEN** the message SHALL appear in every other team member's inbox

### Requirement: Message format
Each mailbox message SHALL contain: `from` (sender agent name), `to` (recipient or "*"), `text` (message body), `timestamp` (unix milliseconds), `read` (boolean), and `type` (string: "message", "shutdown_request", "shutdown_response").

#### Scenario: Standard text message
- **WHEN** an agent sends a regular text message
- **THEN** the message SHALL have `type: "message"` and contain the text body

#### Scenario: Structured shutdown request
- **WHEN** the team lead sends a shutdown request
- **THEN** the message SHALL have `type: "shutdown_request"`
- **THEN** receiving agents SHALL initiate graceful shutdown

### Requirement: Send message tool
The system SHALL provide an `agent_send_message` tool that allows agents to send messages to other team members. The tool SHALL accept `to` (agent name or "*" for broadcast) and `text` parameters.

#### Scenario: Direct message to named agent
- **WHEN** `agent_send_message` is called with `to: "researcher"` and `text: "focus on API docs"`
- **THEN** the message SHALL be delivered to the "researcher" agent's inbox

#### Scenario: Broadcast to all team members
- **WHEN** `agent_send_message` is called with `to: "*"` and `text: "status update needed"`
- **THEN** every team member (except the sender) SHALL receive the message

#### Scenario: Message to non-existent agent
- **WHEN** `agent_send_message` is called with a `to` name that doesn't match any team member
- **THEN** the tool SHALL return an error: `"agent '<name>' not found in team"`

### Requirement: Mailbox persistence
Mailbox messages SHALL be persisted to SQLite. The mailbox table SHALL store messages with foreign keys to the team and agent records.

#### Scenario: Messages survive agent restart
- **WHEN** an agent is stopped and restarted
- **THEN** unread messages in its inbox SHALL still be available

### Requirement: Mailbox polling
Agents SHALL poll their inbox at a configurable interval (default 100ms). When new messages are found, they SHALL be injected into the agent's conversation as user-role messages wrapped in `<teammate-message from="...">` XML tags.

#### Scenario: Agent processes incoming messages
- **WHEN** an agent's mailbox poll finds unread messages
- **THEN** each message SHALL be formatted as `<teammate-message from="sender">text</teammate-message>`
- **THEN** messages SHALL be marked as `read: true` after injection
- **THEN** the agent SHALL be able to respond or take action based on the message content
