## ADDED Requirements

### Requirement: Pipeline evaluation order
The permission checking pipeline SHALL evaluate in this order: (1) deny rules, (2) ask rules, (3) tool-specific checkPermissions, (4) protected-path safety checks, (5) mode-based auto-allow, (6) allow rules, (7) default to ask. The pipeline SHALL short-circuit on the first definitive decision (deny or allow from rules/safety).

#### Scenario: Deny rule short-circuits
- **WHEN** a tool call matches a deny rule
- **THEN** the pipeline SHALL return `deny` immediately without evaluating further steps

#### Scenario: Full pipeline to default
- **WHEN** a tool call matches no rules, has no tool-specific opinion, targets no protected path, and mode is `default_mode`
- **THEN** the pipeline SHALL fall through to step 7 and return `ask`

### Requirement: PermissionDecision result type
The pipeline SHALL return a `PermissionDecision` containing: `behavior` (allow/deny/ask), optional `message` (explanation for ask/deny), and optional `reason` (provenance: rule, mode, safety_check, tool_specific, hook).

#### Scenario: Decision from deny rule
- **WHEN** a deny rule matches
- **THEN** the decision SHALL have `behavior = deny`, `reason.type = rule`, and a message identifying the matched rule

#### Scenario: Decision from mode
- **WHEN** bypass_permissions mode auto-allows a tool
- **THEN** the decision SHALL have `behavior = allow` and `reason.type = mode`

### Requirement: DecisionReason provenance tracking
Each `PermissionDecision` SHALL include a `DecisionReason` indicating the source of the decision. Supported reason types: `rule` (a permission rule matched), `mode` (the permission mode dictated), `safety_check` (a protected-path check triggered), `tool_specific` (tool's own checkPermissions decided), `hook` (a before_tool_call hook decided).

#### Scenario: Rule-based reason
- **WHEN** the decision is caused by a deny rule from `project_settings`
- **THEN** the reason SHALL be `{ type: rule, source: project_settings, rule_value: "Shell(rm:*)" }`

#### Scenario: Safety check reason
- **WHEN** the decision is caused by a protected-path safety check
- **THEN** the reason SHALL be `{ type: safety_check, path: ".git/config" }`

### Requirement: Deny rules evaluated before all else
Deny rules (tool-level) SHALL be checked first in the pipeline. If a deny rule matches the tool name (and content if specified), the tool call SHALL be denied immediately.

#### Scenario: Tool-level deny
- **WHEN** deny rule `"Shell"` exists and a Shell tool call is made
- **THEN** the call SHALL be denied before any other checks run

#### Scenario: Content-level deny
- **WHEN** deny rule `"Shell(rm:*)"` exists and command is `rm -rf /tmp`
- **THEN** the call SHALL be denied
- **AND** `Shell(ls)` SHALL NOT be denied by this rule

### Requirement: Ask rules override allow
Ask rules SHALL be evaluated after deny rules. If an ask rule matches, the decision SHALL be `ask` even if the mode would otherwise auto-allow, EXCEPT in `bypass_permissions` mode where ask rules are respected only for content-specific matches.

#### Scenario: Ask rule in accept_edits mode
- **WHEN** mode is `accept_edits`, ask rule `"Shell(git push:*)"` exists, and command is `git push --force`
- **THEN** the decision SHALL be `ask` (ask rule overrides mode auto-allow)

### Requirement: Mode auto-allow step
After deny rules, ask rules, tool-specific checks, and safety checks, the pipeline SHALL apply mode-based auto-allow: `bypass_permissions` allows all, `accept_edits` allows file tools in working directory, `plan` allows read-only tools, `default_mode` and `dont_ask` do not auto-allow.

#### Scenario: Bypass auto-allows
- **WHEN** mode is `bypass_permissions` and no deny/ask/safety matched
- **THEN** the pipeline SHALL return `allow` with reason `mode`

### Requirement: Allow rules checked after mode
Tool-level allow rules SHALL be checked after mode-based auto-allow. If an allow rule matches, the tool SHALL be allowed.

#### Scenario: Allow rule grants access
- **WHEN** mode is `default_mode` and allow rule `"Read"` exists
- **THEN** Read tool calls SHALL return `allow` with reason `rule`

### Requirement: Dont-ask post-processing
After the pipeline produces a decision, if mode is `dont_ask` and the decision is `ask`, the system SHALL convert it to `deny` with reason `mode`.

#### Scenario: Ask converted to deny in dont_ask
- **WHEN** mode is `dont_ask` and the pipeline returns `ask`
- **THEN** the final decision SHALL be `deny` with reason type `mode`

### Requirement: Hook integration
Before entering the rule pipeline, `before_tool_call` hooks SHALL be dispatched. If any hook blocks the call, the pipeline SHALL return `deny` with reason `hook` without further evaluation.

#### Scenario: Hook blocks tool call
- **WHEN** a `before_tool_call` hook returns non-zero exit code
- **THEN** the decision SHALL be `deny` with reason `{ type: hook }`

### Requirement: Definition filter hides denied tools
Tools that are denied at the tool level (whole-tool deny rule, not content-specific) SHALL be excluded from the tool definitions sent to the LLM, so the model cannot attempt to call them.

#### Scenario: Denied tool hidden from LLM
- **WHEN** deny rule `"Shell"` exists (no content qualifier)
- **THEN** the Shell tool SHALL NOT appear in the tool definitions provided to the model

#### Scenario: Content-specific deny does not hide tool
- **WHEN** deny rule `"Shell(rm:*)"` exists (content-qualified)
- **THEN** the Shell tool SHALL still appear in tool definitions (only specific commands are denied)
