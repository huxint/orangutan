## ADDED Requirements

### Requirement: Permission rule structure
A permission rule SHALL consist of: a `source` (where the rule came from), a `behavior` (allow/deny/ask), a `tool_name` (which tool), and an optional `content` qualifier (pattern to match against tool input).

#### Scenario: Rule with no content qualifier
- **WHEN** a rule is defined as `"Read"` with behavior `allow`
- **THEN** the rule SHALL match all invocations of the `Read` tool regardless of input

#### Scenario: Rule with content qualifier
- **WHEN** a rule is defined as `"Shell(npm install)"` with behavior `allow`
- **THEN** the rule SHALL match only Shell invocations where the command is exactly `npm install`

### Requirement: Rule parsing from string
The system SHALL parse rule strings in the format `ToolName` (whole-tool) or `ToolName(content)` (content-qualified). Parentheses within the content SHALL be supported via escaping.

#### Scenario: Parse whole-tool rule
- **WHEN** the string `"Read"` is parsed
- **THEN** the result SHALL have `tool_name = "Read"` and no content qualifier

#### Scenario: Parse content-qualified rule
- **WHEN** the string `"Shell(git push)"` is parsed
- **THEN** the result SHALL have `tool_name = "Shell"`, content match type `exact`, and pattern `"git push"`

#### Scenario: Parse prefix rule
- **WHEN** the string `"Shell(npm:*)"` is parsed
- **THEN** the result SHALL have `tool_name = "Shell"`, content match type `prefix`, and pattern `"npm"`

#### Scenario: Parse wildcard rule
- **WHEN** the string `"Shell(git * --force)"` is parsed
- **THEN** the result SHALL have `tool_name = "Shell"`, content match type `wildcard`, and pattern `"git * --force"`

### Requirement: Prefix matching
A prefix rule `ToolName(prefix:*)` SHALL match any tool input where the content starts with `prefix` followed by a word boundary (space or end-of-string).

#### Scenario: Prefix match succeeds
- **WHEN** rule is `Shell(npm:*)` and command is `npm install express`
- **THEN** the rule SHALL match

#### Scenario: Prefix match fails on partial word
- **WHEN** rule is `Shell(npm:*)` and command is `npmx something`
- **THEN** the rule SHALL NOT match (no word boundary after prefix)

### Requirement: Wildcard matching
A wildcard rule `ToolName(pattern)` where pattern contains unescaped `*` SHALL match using glob-style matching where `*` matches any sequence of characters.

#### Scenario: Wildcard match
- **WHEN** rule is `Shell(git * --force)` and command is `git push --force`
- **THEN** the rule SHALL match

#### Scenario: Wildcard no match
- **WHEN** rule is `Shell(git * --force)` and command is `git push --no-verify`
- **THEN** the rule SHALL NOT match

### Requirement: Rule sources
The system SHALL support rules from the following sources in descending priority: `cli_arg`, `session`, `local_settings`, `project_settings`, `user_settings`.

#### Scenario: Higher priority source wins
- **WHEN** `cli_arg` has `allow: ["Shell"]` and `user_settings` has `deny: ["Shell"]`
- **THEN** the effective behavior for Shell SHALL be `allow` (cli_arg wins)

### Requirement: Behavior priority within same source
Within the same source, deny rules SHALL take precedence over ask rules, which SHALL take precedence over allow rules.

#### Scenario: Deny beats allow at same source
- **WHEN** `user_settings` has both `allow: ["Shell"]` and `deny: ["Shell(rm:*)"]`
- **THEN** `Shell(rm -rf /)` SHALL be denied and `Shell(ls)` SHALL be allowed

### Requirement: Multi-source rule loading
The system SHALL load rules from: `~/.orangutan/config.json` (user_settings), `.orangutan/settings.json` (project_settings), `.orangutan/settings.local.json` (local_settings), and CLI arguments.

#### Scenario: Project settings file loaded
- **WHEN** `.orangutan/settings.json` contains `{ "permissions": { "deny": ["Shell(rm:*)"] } }`
- **THEN** a deny rule with source `project_settings` SHALL be loaded for `Shell(rm:*)`

### Requirement: Allowed-tools CLI flag
The system SHALL accept `--allowed-tools tool1,tool2` CLI flag that adds allow rules with source `cli_arg`.

#### Scenario: CLI allowed tools
- **WHEN** CLI is invoked with `--allowed-tools Shell,Read`
- **THEN** allow rules for `Shell` and `Read` SHALL be added with source `cli_arg`

### Requirement: Disallowed-tools CLI flag
The system SHALL accept `--disallowed-tools tool1,tool2` CLI flag that adds deny rules with source `cli_arg`.

#### Scenario: CLI disallowed tools
- **WHEN** CLI is invoked with `--disallowed-tools Shell`
- **THEN** a deny rule for `Shell` SHALL be added with source `cli_arg`
