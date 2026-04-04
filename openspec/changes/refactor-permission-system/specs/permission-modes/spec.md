## ADDED Requirements

### Requirement: Permission mode enum
The system SHALL define a `PermissionMode` enum with values: `default_mode`, `accept_edits`, `plan`, `bypass_permissions`, `dont_ask`.

#### Scenario: All modes are defined
- **WHEN** the `PermissionMode` enum is compiled
- **THEN** it SHALL contain exactly 5 values: `default_mode`, `accept_edits`, `plan`, `bypass_permissions`, `dont_ask`

### Requirement: Default mode behavior
In `default_mode`, every tool call that is not explicitly allowed by a rule SHALL require user approval before execution.

#### Scenario: Unapproved tool in default mode
- **WHEN** mode is `default_mode` and a tool call has no matching allow rule
- **THEN** the permission check SHALL return `ask` behavior

#### Scenario: Allowed tool in default mode
- **WHEN** mode is `default_mode` and a tool call matches an allow rule
- **THEN** the permission check SHALL return `allow` behavior

### Requirement: Accept-edits mode behavior
In `accept_edits` mode, file read/write/edit tools targeting paths within the working directory SHALL be auto-approved. Shell commands SHALL still require approval unless explicitly allowed by a rule.

#### Scenario: File edit in accept_edits mode
- **WHEN** mode is `accept_edits` and a FileEdit tool targets a path within the working directory
- **THEN** the permission check SHALL return `allow` behavior without prompting

#### Scenario: Shell command in accept_edits mode
- **WHEN** mode is `accept_edits` and a Shell tool call has no matching allow rule
- **THEN** the permission check SHALL return `ask` behavior

### Requirement: Plan mode behavior
In `plan` mode, only read-only tools SHALL be auto-approved. All write operations SHALL be denied without prompting.

#### Scenario: Read-only tool in plan mode
- **WHEN** mode is `plan` and a tool is marked read-only
- **THEN** the permission check SHALL return `allow` behavior

#### Scenario: Write tool in plan mode
- **WHEN** mode is `plan` and a tool performs write operations
- **THEN** the permission check SHALL return `deny` behavior with message "Plan mode: write operations not allowed"

### Requirement: Bypass-permissions mode behavior
In `bypass_permissions` mode, all tool calls SHALL be auto-approved EXCEPT those blocked by deny rules or safety checks for protected paths.

#### Scenario: Normal tool in bypass mode
- **WHEN** mode is `bypass_permissions` and a tool call has no deny rule and targets no protected path
- **THEN** the permission check SHALL return `allow` behavior

#### Scenario: Denied tool in bypass mode
- **WHEN** mode is `bypass_permissions` and a tool call matches a deny rule
- **THEN** the permission check SHALL return `deny` behavior

#### Scenario: Protected path in bypass mode
- **WHEN** mode is `bypass_permissions` and a tool targets a protected path (`.git/`, `.orangutan/`)
- **THEN** the permission check SHALL return `ask` behavior (safety check is bypass-immune)

### Requirement: Dont-ask mode behavior
In `dont_ask` mode, any tool call that would normally produce an `ask` decision SHALL be converted to `deny`. No interactive prompts SHALL occur.

#### Scenario: Tool that would ask in dont_ask mode
- **WHEN** mode is `dont_ask` and a tool call would normally return `ask`
- **THEN** the permission check SHALL return `deny` behavior with reason indicating mode-based denial

### Requirement: Mode from CLI flag
The system SHALL accept a `--permission-mode <mode>` CLI flag that sets the initial permission mode for the session.

#### Scenario: CLI sets permission mode
- **WHEN** the CLI is invoked with `--permission-mode accept_edits`
- **THEN** the session SHALL start with `accept_edits` mode

### Requirement: Bypass permissions CLI flag
The system SHALL accept a `--dangerously-skip-permissions` CLI flag that sets mode to `bypass_permissions`.

#### Scenario: Skip permissions flag
- **WHEN** the CLI is invoked with `--dangerously-skip-permissions`
- **THEN** the session SHALL start with `bypass_permissions` mode
- **AND** this SHALL take precedence over `--permission-mode` if both are specified

### Requirement: Default mode from config
The system SHALL read `permissions.default_mode` from the config file to set the default permission mode when no CLI flag overrides it.

#### Scenario: Config sets default mode
- **WHEN** config contains `"permissions": { "default_mode": "accept_edits" }` and no CLI mode flag is given
- **THEN** the session SHALL start with `accept_edits` mode

#### Scenario: CLI overrides config default mode
- **WHEN** config contains `"default_mode": "default"` and CLI has `--permission-mode bypass_permissions`
- **THEN** the session SHALL start with `bypass_permissions` mode
