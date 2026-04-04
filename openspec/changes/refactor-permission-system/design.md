## Context

Orangutan's current permission system is a simple three-layer model: `ToolSandboxMode` (isolated/workspace_write/disabled), `ToolApprovalPolicy` (ask/allow/deny), and flat string lists for allowed/denied tools and shell commands. Permissions are evaluated in a single `evaluate_tool_permission()` function with substring matching for shell commands. There are no permission modes, no content-qualified rules, no tool-specific permission logic, and no multi-source rule resolution.

Claude Code's permission system is a sophisticated pipeline with: 5 permission modes controlling default behavior, pattern-based rules with content qualifiers and wildcard/prefix matching, rules sourced from 7+ locations with priority resolution, tool-specific `checkPermissions()` methods, protected-path safety checks that cannot be bypassed, and immutable permission context state management.

The existing orangutan code to be replaced spans: `permissions.hpp/.cpp` (types + evaluation), `tool-context.hpp` (approval callback), `config-sections-core.cpp` (config parsing), `config.hpp` (settings struct), `config-builder.cpp` (inheritance), `runtime-loader.cpp` (policy application), and the CLI/channel/web approval flows.

## Goals / Non-Goals

**Goals:**
- Adopt Claude Code's permission mode system (default, accept_edits, plan, bypass_permissions, dont_ask)
- Implement rule-based permission matching with content qualifiers (`Shell(npm:*)`, `Shell(git *)`)
- Support multi-source rule resolution with defined priority order
- Add tool-specific `checkPermissions()` virtual method to `ToolDefinition`
- Add protected-path safety checks that are bypass-immune
- Add CLI flags for permission mode and tool allow/deny
- Maintain existing approval callback mechanism (CLI/channel/web) adapted to new decision types
- Clean config migration path

**Non-Goals:**
- Auto mode / AI classifier-based permission decisions (Claude Code's `auto` mode uses a transcript classifier — too complex, not needed now)
- Bubble mode (internal to Claude Code's subagent architecture)
- Remote killswitches via feature flags (GrowthBook/Statsig integration)
- Denial tracking and auto-mode fallback logic
- Tree-sitter AST parsing for shell command security analysis
- React/UI layer (orangutan uses different UI paradigm)
- PowerShell-specific permission handling

## Decisions

### 1. Permission mode as an enum, not a string union

Use `enum class PermissionMode { default_mode, accept_edits, plan, bypass_permissions, dont_ask }` rather than string-based mode matching. C++ enums give compile-time safety and `magic_enum` integration for serialization.

**Alternative**: String-based modes like Claude Code's TypeScript union types. Rejected because C++ enums are superior for this — exhaustive switch, no typos, zero runtime cost.

### 2. PermissionRule as a struct with parsed components

```cpp
struct PermissionRule {
    PermissionRuleSource source;
    PermissionBehavior behavior;  // allow, deny, ask
    std::string tool_name;
    std::optional<RuleContent> content;  // nullopt = whole-tool rule
};

struct RuleContent {
    RuleMatchType match_type;  // exact, prefix, wildcard
    std::string pattern;
};
```

Parse rules once at load time rather than re-parsing on every check. Claude Code re-parses rule strings on each evaluation — we avoid that overhead.

**Alternative**: Store rules as raw strings and parse on demand. Rejected for performance — rules are evaluated on every tool call.

### 3. ToolPermissionContext as an immutable value type

```cpp
struct ToolPermissionContext {
    PermissionMode mode;
    std::vector<PermissionRule> allow_rules;
    std::vector<PermissionRule> deny_rules;
    std::vector<PermissionRule> ask_rules;
    bool is_bypass_available;
    std::vector<std::string> additional_directories;
};
```

New context is created on mode changes or rule updates — the old context is not mutated. This matches Claude Code's approach and prevents race conditions in multi-threaded approval flows.

**Alternative**: Mutable context with locks. Rejected — immutable is simpler and safer for concurrent access from channel/web handlers.

### 4. Permission checking pipeline order

Follow Claude Code's proven pipeline:
1. Deny rules (tool-level) → hard deny
2. Ask rules (tool-level) → mark as ask
3. Tool-specific `checkPermissions()` → may return allow/deny/ask/passthrough
4. Safety checks for protected paths → bypass-immune ask
5. Mode-based auto-allow (bypass_permissions → allow, accept_edits for file tools → allow)
6. Tool-level allow rules → allow
7. Default → ask

This order ensures deny always wins, safety checks can't be bypassed, and mode-based behavior is a late-stage convenience.

**Alternative**: Check allow rules before tool-specific permissions. Rejected — tool-specific checks may detect dangerous patterns even for broadly-allowed tools.

### 5. Rule sources and priority

Sources in descending priority:
1. `cli_arg` — `--allowed-tools` / `--disallowed-tools` flags
2. `session` — runtime-added rules (e.g., user grants "always allow" during session)
3. `local_settings` — `.orangutan/settings.local.json` (gitignored)
4. `project_settings` — `.orangutan/settings.json` (committed)
5. `user_settings` — `~/.orangutan/config.json` (global)

Within same source, deny > ask > allow. Across sources, higher-priority source wins for the same tool+content combination.

**Alternative**: Flat merge like current system. Rejected — multi-source resolution is essential for project-level overrides.

### 6. Tool-specific checkPermissions via virtual method

Add `virtual PermissionResult check_permissions(const ToolUse& call, const ToolPermissionContext& ctx) const` to `ToolDefinition`. Default implementation returns `passthrough`. Shell tool overrides with command-level rule matching and path validation. File tools override with path-based checks.

**Alternative**: Central switch statement in the permission evaluator. Rejected — violates open/closed principle; each tool owns its permission logic.

### 7. Protected paths are hardcoded, not configurable

Safety checks for `.git/`, `.orangutan/`, `.claude/`, and shell config files (`.bashrc`, `.zshrc`, `.profile`) are hardcoded and bypass-immune (always prompt even in bypass_permissions mode). This matches Claude Code's approach.

**Alternative**: Make protected paths configurable. Rejected — configurable safety checks can be accidentally disabled. These are security-critical.

### 8. Keep sandbox as separate infrastructure

Sandbox (bwrap) execution is orthogonal to the permission system. A tool can be allowed by permissions but still sandboxed. The sandbox config moves to a separate `SandboxConfig` on the tool context, not part of `ToolPermissionContext`.

**Alternative**: Keep sandbox in permission settings as before. Rejected — sandboxing is an execution concern, not an authorization concern. Mixing them complicates both.

### 9. Config format uses arrays of rule strings

```json
{
  "permissions": {
    "default_mode": "default",
    "allow": ["Read", "Glob", "Grep"],
    "deny": ["Shell(rm -rf:*)"],
    "ask": ["Shell(git push:*)"]
  }
}
```

Matches Claude Code's format closely. The old `shell_approval_policy`, `sandbox_mode`, `allowed_tools`, `denied_tools`, `denied_shell_commands` fields are all replaced.

### 10. PermissionDecision as a tagged variant

```cpp
struct PermissionDecision {
    PermissionBehavior behavior;
    std::optional<std::string> message;     // for ask/deny: explanation
    std::optional<DecisionReason> reason;   // why this decision was made
    std::optional<nlohmann::json> updated_input;  // for allow: modified input
};
```

The `DecisionReason` tracks provenance: `rule`, `mode`, `safety_check`, `tool_specific`, `hook`. This is critical for debugging and for the UI to show users why a prompt appeared.

## Risks / Trade-offs

- **[Breaking config format]** → Provide a `migrate-config` CLI command that reads old format and writes new format. Document the migration in release notes.
- **[Complexity increase]** → The pipeline is more complex than the current flat check. Mitigated by clear separation of concerns and comprehensive tests.
- **[Shell command matching without tree-sitter]** → We skip Claude Code's AST-based shell parsing. Mitigated by prefix/wildcard matching which covers most practical cases. Can add tree-sitter later if needed.
- **[No auto mode]** → Missing Claude Code's classifier-based auto-allow. This is intentional (non-goal). Can be added later without architectural changes.
- **[Multi-source settings files]** → Users may be confused about which file takes precedence. Mitigated by a `--show-permissions` debug flag that dumps effective rules with sources.
