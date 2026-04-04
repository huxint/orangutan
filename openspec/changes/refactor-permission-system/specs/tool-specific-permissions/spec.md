## ADDED Requirements

### Requirement: Tool checkPermissions virtual method
`ToolDefinition` SHALL declare a virtual method `check_permissions(const ToolUse& call, const ToolPermissionContext& ctx) -> PermissionResult`. The default implementation SHALL return `passthrough`. Tools MAY override this to provide tool-specific permission logic.

#### Scenario: Default tool returns passthrough
- **WHEN** a tool that does not override `check_permissions` is evaluated
- **THEN** it SHALL return `passthrough`, deferring to the pipeline's remaining steps

#### Scenario: Tool overrides with deny
- **WHEN** a tool's `check_permissions` returns `deny` with a message
- **THEN** the pipeline SHALL honor the denial and not evaluate further steps

### Requirement: Shell tool command-level permission checking
The Shell tool's `check_permissions` SHALL extract the command string from the tool input and evaluate it against content-qualified rules (exact, prefix, wildcard matching). For compound commands (`cmd1 && cmd2`), each subcommand SHALL be checked individually — a deny on any subcommand SHALL deny the entire command.

#### Scenario: Shell allow rule with prefix match
- **WHEN** allow rule `Shell(npm:*)` exists and command is `npm test`
- **THEN** the Shell tool's `check_permissions` SHALL return `allow`

#### Scenario: Compound command with one denied subcommand
- **WHEN** deny rule `Shell(rm:*)` exists and command is `ls && rm -rf /tmp`
- **THEN** the Shell tool's `check_permissions` SHALL return `deny` (rm subcommand denied)

#### Scenario: All subcommands allowed
- **WHEN** allow rule `Shell(git:*)` exists and command is `git add . && git commit -m "msg"`
- **THEN** the Shell tool's `check_permissions` SHALL return `allow`

### Requirement: Shell tool path validation
The Shell tool's `check_permissions` SHALL validate that commands operating on file paths target paths within the working directory or additional directories. Commands targeting paths outside allowed directories SHALL return `ask`.

#### Scenario: Command targets path outside workspace
- **WHEN** working directory is `/home/user/project` and command is `cat /etc/passwd`
- **THEN** the Shell tool's `check_permissions` SHALL return `ask` with a message about path outside workspace

### Requirement: Protected-path safety checks
The system SHALL define a set of protected paths that trigger `ask` behavior regardless of permission mode (bypass-immune). Protected paths SHALL include: `.git/` directory contents, `.orangutan/` directory contents, `.claude/` directory contents, shell config files (`.bashrc`, `.zshrc`, `.profile`, `.bash_profile`).

#### Scenario: Write to .git in bypass mode
- **WHEN** mode is `bypass_permissions` and a FileWrite targets `.git/config`
- **THEN** the permission check SHALL return `ask` (safety check is bypass-immune)

#### Scenario: Shell modifies .orangutan in bypass mode
- **WHEN** mode is `bypass_permissions` and command is `rm -rf .orangutan/`
- **THEN** the permission check SHALL return `ask`

#### Scenario: Read from .git is allowed
- **WHEN** a FileRead targets `.git/HEAD`
- **THEN** the safety check SHALL NOT trigger (reads are safe; only writes/deletes are protected)

### Requirement: File tool path-based permissions
File read/write/edit tools SHALL implement `check_permissions` to validate that target paths are within the working directory or additional directories. Paths outside allowed directories SHALL return `ask`.

#### Scenario: File edit within workspace
- **WHEN** FileEdit targets `/home/user/project/src/main.cpp` and working directory is `/home/user/project`
- **THEN** `check_permissions` SHALL return `passthrough` (no objection)

#### Scenario: File edit outside workspace
- **WHEN** FileEdit targets `/etc/hosts` and working directory is `/home/user/project`
- **THEN** `check_permissions` SHALL return `ask` with message about path outside workspace

### Requirement: Read-only tool identification
Each tool SHALL declare whether it is read-only via an `is_read_only()` virtual method. Read-only tools SHALL be auto-approved in `plan` mode and SHALL be given preference in `accept_edits` mode.

#### Scenario: Read tool is read-only
- **WHEN** `is_read_only()` is called on the Read tool
- **THEN** it SHALL return `true`

#### Scenario: Shell tool is not read-only
- **WHEN** `is_read_only()` is called on the Shell tool
- **THEN** it SHALL return `false`

#### Scenario: Glob tool is read-only
- **WHEN** `is_read_only()` is called on the Glob tool
- **THEN** it SHALL return `true`

### Requirement: Tool definition filter respects permissions
The tool definition filter SHALL exclude tools from the LLM's tool list when: (1) a whole-tool deny rule exists, or (2) mode is `plan` and the tool is not read-only. Content-specific deny rules SHALL NOT hide the tool.

#### Scenario: Plan mode hides write tools
- **WHEN** mode is `plan` and the Shell tool is not read-only
- **THEN** Shell SHALL NOT appear in the tool definitions sent to the model

#### Scenario: Plan mode shows read tools
- **WHEN** mode is `plan` and the Read tool is read-only
- **THEN** Read SHALL appear in the tool definitions
