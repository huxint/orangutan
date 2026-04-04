## ADDED Requirements

### Requirement: ToolPermissionContext struct
The system SHALL define a `ToolPermissionContext` struct containing: `mode` (PermissionMode), `allow_rules` (vector of PermissionRule), `deny_rules` (vector of PermissionRule), `ask_rules` (vector of PermissionRule), `is_bypass_available` (bool), and `additional_directories` (vector of string paths).

#### Scenario: Context contains all rule categories
- **WHEN** a `ToolPermissionContext` is constructed with rules from multiple sources
- **THEN** the context SHALL have separate vectors for allow, deny, and ask rules with source information preserved on each rule

### Requirement: Immutable context updates
The `ToolPermissionContext` SHALL be treated as immutable. Mode changes or rule additions SHALL produce a new context instance rather than mutating the existing one.

#### Scenario: Mode change creates new context
- **WHEN** the user changes mode from `default_mode` to `accept_edits`
- **THEN** a new `ToolPermissionContext` SHALL be created with the updated mode
- **AND** the previous context SHALL remain unchanged

#### Scenario: Session rule addition creates new context
- **WHEN** a user grants "always allow" for a tool during the session
- **THEN** a new `ToolPermissionContext` SHALL be created with the additional allow rule (source `session`)

### Requirement: Rule loading from multiple sources
At initialization, the system SHALL load permission rules from all available settings sources: user settings (`~/.orangutan/config.json`), project settings (`.orangutan/settings.json`), local settings (`.orangutan/settings.local.json`), and CLI arguments. Each rule SHALL retain its source.

#### Scenario: Rules loaded from all sources
- **WHEN** user settings has `allow: ["Read"]`, project settings has `deny: ["Shell(rm:*)"]`, and CLI has `--allowed-tools Shell`
- **THEN** the context SHALL contain: allow rule `Read` (source: user_settings), deny rule `Shell(rm:*)` (source: project_settings), allow rule `Shell` (source: cli_arg)

### Requirement: Initialization from config and CLI
The system SHALL initialize `ToolPermissionContext` by: (1) reading the config file's permissions section, (2) reading project/local settings files, (3) parsing CLI flags, (4) combining all rules with their sources, (5) resolving the initial mode from CLI flag or config default.

#### Scenario: Full initialization
- **WHEN** config has `default_mode: "default"`, `allow: ["Read"]` and CLI has `--permission-mode accept_edits`
- **THEN** the context SHALL have mode `accept_edits` (CLI wins), allow rule `Read` (source: user_settings)

### Requirement: Session rule persistence
Rules added during a session with source `session` SHALL exist only in memory for the current session. They SHALL NOT be written to any settings file.

#### Scenario: Session rules are ephemeral
- **WHEN** a user grants "always allow Shell" during a session and the session ends
- **THEN** the next session SHALL NOT have the Shell allow rule unless it exists in a settings file

### Requirement: Settings file persistence
When the user permanently grants or denies a tool (via "always allow" or "always deny" prompt responses), the system SHALL persist the rule to the appropriate settings file based on user choice (user_settings for global, project_settings or local_settings for project-scoped).

#### Scenario: Persist allow rule to user settings
- **WHEN** user responds "Always allow" for the Read tool and chooses global scope
- **THEN** the rule `"Read"` SHALL be added to the `allow` array in `~/.orangutan/config.json`

### Requirement: Context accessible from tool execution
The `ToolPermissionContext` SHALL be accessible from `ToolRuntimeContext` so that tool implementations can read the current mode and rules when needed.

#### Scenario: Tool accesses permission mode
- **WHEN** a tool's `check_permissions()` method is called
- **THEN** it SHALL receive the current `ToolPermissionContext` as a parameter
