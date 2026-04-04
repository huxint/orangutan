## Why

The current permission system is a flat, config-driven model with a single `ToolApprovalPolicy` (ask/allow/deny) and a `ToolSandboxMode` enum. It lacks permission modes, rule-based matching with content qualifiers, multi-source rule resolution, and tool-specific permission logic. Claude Code's permission architecture is significantly more capable — supporting rich permission modes (default, acceptEdits, plan, bypassPermissions, dontAsk), pattern-based rules with content qualifiers (`Bash(npm:*)`, `Shell(git *)`), multi-layered rule sources with priority resolution, tool-specific `checkPermissions()` methods, protected-path safety checks, and session-level permission state management. Adopting this architecture will bring orangutan's permission model to parity with Claude Code, enabling fine-grained control over tool access and safer defaults.

## What Changes

- **BREAKING**: Remove `ToolSandboxMode` enum and bwrap-based sandboxing from the permission system (sandbox becomes orthogonal infrastructure, not a permission concept)
- **BREAKING**: Remove `ToolApprovalPolicy` enum — replaced by `PermissionBehavior` (allow/deny/ask) and `PermissionMode`
- **BREAKING**: Remove `ToolPermissionSettings` struct — replaced by `ToolPermissionContext` with immutable rule sets and mode
- **BREAKING**: Config format changes: `permissions.sandbox_mode` and `permissions.shell_approval_policy` removed; replaced by `permissions.allow[]`, `permissions.deny[]`, `permissions.ask[]`, `permissions.default_mode`
- Add `PermissionMode` enum: `default`, `accept_edits`, `plan`, `bypass_permissions`, `dont_ask`
- Add `PermissionBehavior` enum: `allow`, `deny`, `ask`
- Add rule parsing system supporting `ToolName(content)`, `ToolName(prefix:*)`, `ToolName(glob *)` patterns
- Add multi-source rule resolution: user settings, project settings (.orangutan/settings.json), local settings, CLI args, session rules
- Add `checkPermissions()` virtual method to `ToolDefinition` for tool-specific permission logic
- Add protected-path safety checks (bypass-immune) for `.git/`, `.claude/`, `.orangutan/`, shell configs
- Add CLI flags: `--permission-mode`, `--allowed-tools`, `--disallowed-tools`, `--dangerously-skip-permissions`
- Add pipeline-based permission checking: deny rules → ask rules → tool-specific checks → safety checks → mode-based auto-allow → default ask
- Rework execution guard and definition filter to use new `ToolPermissionContext`
- Update channel/web/CLI approval callbacks to work with new permission decision types

## Capabilities

### New Capabilities
- `permission-modes`: Permission mode system (default, accept_edits, plan, bypass_permissions, dont_ask) with mode-based auto-allow logic and CLI flag support
- `permission-rules`: Rule-based permission matching with content qualifiers, wildcard/prefix patterns, multi-source priority resolution, and rule parsing
- `permission-checking-pipeline`: Multi-step permission checking flow — deny → ask → tool-specific → safety → mode → default, with `PermissionDecision` result types and decision reasons
- `permission-state`: Immutable `ToolPermissionContext` state management, rule loading from multiple settings sources, session rule updates, and settings persistence
- `tool-specific-permissions`: Virtual `checkPermissions()` on tools, shell command permission evaluation with subcommand splitting, and protected-path safety checks

### Modified Capabilities

## Impact

- **Config breaking change**: All users must migrate `permissions` config section to new format
- **Core files rewritten**: `src/tools/registry/permissions.hpp/.cpp`, `src/tools/registry/tool-registry.cpp`, `src/tools/registry/tool-context.hpp`
- **Config parser rewritten**: `src/config/config-sections-core.cpp`, `src/config/config.hpp`
- **Bootstrap updated**: `src/bootstrap/agent-runtime.cpp`, `config-builder.cpp`, `cli-options.hpp/.cpp`
- **All approval callbacks updated**: CLI, channel, web approval flows adapt to new `PermissionDecision` types
- **Tool registration updated**: `src/tools/register.cpp`, shell tool, script tool, file tools gain `checkPermissions()`
- **Tests**: All permission-related tests must be rewritten
- **New settings files**: Support for `.orangutan/settings.json` (project-level) and `.orangutan/settings.local.json`
