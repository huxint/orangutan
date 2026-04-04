## 1. Core Types and Enums

- [x] 1.1 Define `PermissionMode` enum (default_mode, accept_edits, plan, bypass_permissions, dont_ask) with magic_enum serialization in new `src/permissions/permission-types.hpp`
- [x] 1.2 Define `PermissionBehavior` enum (allow, deny, ask) and `PermissionResult` struct (behavior + passthrough variant) in `src/permissions/permission-types.hpp`
- [x] 1.3 Define `RuleMatchType` enum (exact, prefix, wildcard), `RuleContent` struct, `PermissionRuleSource` enum (cli_arg, session, local_settings, project_settings, user_settings), and `PermissionRule` struct in `src/permissions/permission-types.hpp`
- [x] 1.4 Define `DecisionReason` variant (rule, mode, safety_check, tool_specific, hook) and `PermissionDecision` struct (behavior, message, reason) in `src/permissions/permission-types.hpp`
- [x] 1.5 Define `ToolPermissionContext` struct (mode, allow/deny/ask rule vectors, is_bypass_available, additional_directories) in `src/permissions/permission-types.hpp`

## 2. Rule Parsing

- [x] 2.1 Implement `parse_permission_rule(string, PermissionBehavior, PermissionRuleSource) -> PermissionRule` in `src/permissions/rule-parser.hpp/.cpp` — parse `ToolName`, `ToolName(exact)`, `ToolName(prefix:*)`, `ToolName(glob *)` formats
- [x] 2.2 Implement prefix matching: `matches_prefix(pattern, input) -> bool` — word-boundary aware prefix match
- [x] 2.3 Implement wildcard matching: `matches_wildcard(pattern, input) -> bool` — glob `*` to regex conversion
- [x] 2.4 Implement `matches_rule(PermissionRule, tool_name, content) -> bool` — unified rule matching dispatch
- [x] 2.5 Write unit tests for rule parsing and matching: exact, prefix, wildcard, edge cases (escaped parens, empty content, no content qualifier)

## 3. Permission Checking Pipeline

- [x] 3.1 Implement `evaluate_permission(ToolUse, ToolPermissionContext) -> PermissionDecision` pipeline in `src/permissions/permission-evaluator.hpp/.cpp` — the 7-step pipeline (deny → ask → tool-specific → safety → mode → allow → default ask)
- [x] 3.2 Implement deny-rule lookup: `find_deny_rule(tool_name, content, rules) -> optional<PermissionRule>`
- [x] 3.3 Implement ask-rule lookup: `find_ask_rule(tool_name, content, rules) -> optional<PermissionRule>`
- [x] 3.4 Implement allow-rule lookup: `find_allow_rule(tool_name, content, rules) -> optional<PermissionRule>`
- [x] 3.5 Implement mode-based auto-allow logic: `evaluate_mode(PermissionMode, ToolUse, is_read_only) -> optional<PermissionDecision>`
- [x] 3.6 Implement dont_ask post-processing: convert ask → deny when mode is dont_ask
- [x] 3.7 Write unit tests for the full pipeline: each step in isolation and integration scenarios

## 4. Protected-Path Safety Checks

- [x] 4.1 Implement `is_protected_path(path) -> bool` and `check_safety(ToolUse, ToolPermissionContext) -> optional<PermissionDecision>` in `src/permissions/safety-checks.hpp/.cpp` — hardcoded checks for .git/, .orangutan/, .claude/, shell configs
- [x] 4.2 Distinguish read vs write operations: reads of protected paths are allowed, only writes/deletes trigger safety prompts
- [x] 4.3 Write unit tests for safety checks: bypass-immune behavior, read vs write, various protected paths

## 5. Tool-Specific Permission Methods

- [ ] 5.1 Add `virtual PermissionResult check_permissions(const ToolUse&, const ToolPermissionContext&) const` to `ToolDefinition` with default `passthrough` return
- [ ] 5.2 Add `virtual bool is_read_only() const` to `ToolDefinition` with default `false` return
- [ ] 5.3 Implement Shell tool `check_permissions`: extract command, split compound commands (&&, ||, ;), evaluate each subcommand against content-qualified rules, validate paths
- [ ] 5.4 Implement file tool `check_permissions` for FileRead/FileWrite/FileEdit: validate target path is within working directory or additional directories
- [ ] 5.5 Mark read-only tools: FileRead, Glob, Grep return `true` from `is_read_only()`
- [ ] 5.6 Write unit tests for tool-specific permissions: shell compound commands, path validation, read-only identification

## 6. Permission State Management

- [x] 6.1 Implement `initialize_permission_context(Config, CLIOptions) -> ToolPermissionContext` in `src/permissions/permission-state.hpp/.cpp` — load rules from all sources, resolve initial mode
- [x] 6.2 Implement `apply_permission_update(ToolPermissionContext, PermissionUpdate) -> ToolPermissionContext` — immutable context updates for mode changes and rule additions
- [x] 6.3 Implement settings file loading: `load_rules_from_file(path, PermissionRuleSource) -> vector<PermissionRule>` for user/project/local settings
- [x] 6.4 Implement settings file persistence: `persist_rule(PermissionRule, target_file)` — write allow/deny rules to the appropriate settings JSON file
- [x] 6.5 Write unit tests for state management: initialization, immutable updates, file loading, persistence

## 7. Config Format Migration

- [ ] 7.1 Update `config-sections-core.cpp`: replace `apply_permissions_object()` — parse new format (`default_mode`, `allow[]`, `deny[]`, `ask[]`) while still accepting old keys with deprecation warnings
- [ ] 7.2 Update `config.hpp`: replace `ToolPermissionSettings` field with `PermissionConfig` struct containing the new format fields, keep `SandboxConfig` separate
- [ ] 7.3 Update `config.example.json` with new permissions format and examples
- [ ] 7.4 Support `.orangutan/settings.json` and `.orangutan/settings.local.json` as additional settings sources
- [ ] 7.5 Write config parsing tests for new format and backward-compatible parsing of old format

## 8. CLI Flags

- [ ] 8.1 Add `--permission-mode <mode>` CLI flag to `cli-options.hpp/.cpp`
- [ ] 8.2 Add `--dangerously-skip-permissions` CLI flag (sets mode to bypass_permissions)
- [ ] 8.3 Add `--allowed-tools <list>` and `--disallowed-tools <list>` CLI flags (comma-separated tool specs)
- [ ] 8.4 Implement CLI flag precedence: `--dangerously-skip-permissions` > `--permission-mode` > config `default_mode`
- [ ] 8.5 Write CLI flag tests

## 9. Integration — Execution Guard and Definition Filter

- [ ] 9.1 Rewrite `apply_permission_policy()` in `runtime-loader.cpp` to use new `evaluate_permission()` pipeline and `ToolPermissionContext`
- [ ] 9.2 Update execution guard to produce `PermissionDecision` and invoke approval callback for `ask` decisions
- [ ] 9.3 Update definition filter to hide whole-tool denied tools and write tools in plan mode
- [ ] 9.4 Update `ToolRuntimeContext` (`tool-context.hpp`) to hold `ToolPermissionContext*` instead of old `ToolPermissionSettings*` and `approval_callback`
- [ ] 9.5 Write integration tests: guard blocks denied tools, filter hides tools, approval callback invoked for ask

## 10. Approval Callback Adaptation

- [ ] 10.1 Update CLI approval callback (`cli-options.cpp`) to receive `PermissionDecision` and display decision reason and rule info
- [ ] 10.2 Update channel approval coordinator (`channel-serve.cpp`) to format `PermissionDecision` in approval request messages
- [ ] 10.3 Update web approval flow (`web-routes.cpp`, `chat-routes.cpp`) to send `PermissionDecision` details in SSE approval_request events and handle responses
- [ ] 10.4 Write tests for each approval callback with the new decision types

## 11. Bootstrap and Wiring

- [ ] 11.1 Update `agent-runtime.hpp/.cpp`: replace `ToolPermissionSettings` with `ToolPermissionContext` in `AgentRuntimeBuildInput` and runtime construction
- [ ] 11.2 Update `config-builder.cpp`: `build_effective_agents()` constructs `ToolPermissionContext` per agent with proper inheritance
- [ ] 11.3 Update `bootstrap.cpp`: wire CLI flags → config → `initialize_permission_context()` → agent runtime
- [ ] 11.4 Update `register.cpp`: pass `ToolPermissionContext*` to tools instead of `ToolPermissionSettings*`

## 12. Remove Old Permission Code

- [ ] 12.1 Remove `ToolSandboxMode`, `ToolApprovalPolicy`, `ToolPermissionSettings` from old `permissions.hpp`
- [ ] 12.2 Remove old `evaluate_tool_permission()`, `evaluate_shell_command_permission()`, `blocked_shell_command_pattern()` from old `permissions.cpp`
- [ ] 12.3 Remove old `is_tool_allowed()` from `config.cpp`
- [ ] 12.4 Remove old permission config fields from `config.hpp` (`shell_approval`, `denied_shell_commands`, etc.)
- [ ] 12.5 Update xmake build targets: add new `src/permissions/` source files, remove old permissions sources if fully replaced

## 13. Test Rewrite

- [ ] 13.1 Rewrite `runtime-agent-runtime-test.cpp` permission tests for new types and context
- [ ] 13.2 Rewrite `channel-serve-test.cpp` approval tests for new decision types
- [ ] 13.3 Rewrite `web-routes-test.cpp` and `web-chat-test.cpp` approval tests
- [ ] 13.4 Rewrite `tool-registry-test.cpp` permission-related tests
- [ ] 13.5 Rewrite config tests in `config-test.cpp` and `config-save-test.cpp` for new format
- [ ] 13.6 Full build and all tests pass
