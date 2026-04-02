# Prompt Engineering Overhaul Plan

## Overview

Rewrite the entire prompt system: system prompts, tool descriptions, safety instructions, and environment injection. Adapted from Claude Code's production-proven patterns, localized for Orangutan's architecture.

## Changes

### 1. New file: `src/prompt/system-prompt-sections.hpp`

A header with free functions that return each prompt section as `std::string`. No classes needed — just pure functions the builder calls.

Sections:
- `identity_section()` — role framing ("You are Orangutan...")
- `system_behavior_section()` — markdown output, hooks, tool permission semantics, compression notice
- `task_guidelines_section()` — coding best practices, security, code style, no over-engineering
- `safety_actions_section()` — reversibility analysis, destructive command warnings, "measure twice cut once"
- `tool_usage_section(const ToolRegistry&)` — dynamic: prefers read/write/edit over shell, parallel tool calls guidance
- `tone_style_section()` — no emojis, concise, file_path:line_number references
- `output_efficiency_section()` — "go straight to the point", lead with action
- `environment_section(workspace, model_name)` — cwd, git status, platform, OS, model info
- `security_instruction()` — cyber risk boundary (CTF/pentest OK, malicious refuse)

### 2. New file: `src/prompt/system-prompt-sections.cpp`

Implementation with all prompt text as `constexpr string_view` or inline strings.

### 3. Modify: `src/agent/agent-loop.cpp`

- Remove the 3-line `default_system_prompt` constant
- Rewrite `build_system_prompt()` to call the new section functions and compose them
- Add git status detection (call `git status --short`, `git branch`, `git log --oneline -5` via popen or std::system)
- Inject environment info dynamically

### 4. Modify: `src/agent/agent-loop.hpp`

- Add `std::string workspace_root_` member (passed from constructor for env detection)
- Add `std::string model_name_` member (for environment section)
- Update constructor signature

### 5. Modify: `src/bootstrap/agent-runtime.cpp`

- Pass workspace_root and model name to AgentLoop constructor

### 6. Modify tool descriptions (enhanced prompts):

**`src/tools/shell/shell-tool.cpp`** — Shell tool gets a MUCH longer description:
- "Avoid using shell for file operations when dedicated tools exist"
- "Use read tool instead of cat/head/tail"
- "Use edit tool instead of sed/awk"
- "Use write tool instead of echo redirection"
- Git command guidance (prefer new commits, avoid --no-verify, avoid destructive ops)
- Background command guidance
- Multiple command chaining guidance (&&, ;, parallel calls)

**`src/tools/file-read/file-read-tool.cpp`** — Enhanced:
- "Must use absolute or workspace-relative paths"
- "Results use cat -n format with line numbers starting at 1"
- "Can read binary files (detected automatically)"
- "For large files, use offset/limit pagination"
- "Can read multiple files in one call using paths array"

**`src/tools/file-write/file-write-tool.cpp`** — Enhanced:
- "Prefer the edit tool for modifying existing files — it only sends the diff"
- "Only use write for creating new files or complete rewrites"
- "You MUST use the read tool first before overwriting an existing file"
- "NEVER create documentation files unless explicitly requested"

**`src/tools/file-edit/file-edit-tool.cpp`** — Enhanced:
- "You must read a file before editing it"
- "ALWAYS prefer editing existing files. NEVER write new files unless required"
- "Preserve exact indentation from the read output"

**`src/tools/memory/memory-tool.cpp`** — Enhanced remember description:
- "Store durable facts, preferences, or project context for future conversations"
- "Use meaningful keys like 'project.lang' or 'preference.style'"

### 7. Modify: `src/bootstrap/identity.cpp`

- Enhance `append_subagent_prompt_guidance()` with clearer delegation instructions

## Files changed (summary)

| File | Action |
|------|--------|
| `src/prompt/system-prompt-sections.hpp` | **Create** |
| `src/prompt/system-prompt-sections.cpp` | **Create** |
| `src/agent/agent-loop.hpp` | Modify (add workspace/model members) |
| `src/agent/agent-loop.cpp` | Modify (rewrite build_system_prompt, remove default_system_prompt) |
| `src/bootstrap/agent-runtime.cpp` | Modify (pass workspace/model to AgentLoop) |
| `src/tools/shell/shell-tool.cpp` | Modify (enhanced description) |
| `src/tools/file-read/file-read-tool.cpp` | Modify (enhanced description) |
| `src/tools/file-write/file-write-tool.cpp` | Modify (enhanced description) |
| `src/tools/file-edit/file-edit-tool.cpp` | Modify (enhanced description) |
| `src/tools/memory/memory-tool.cpp` | Modify (enhanced description) |
| `src/bootstrap/identity.cpp` | Modify (enhanced subagent guidance) |
| `tests/bootstrap/identity-test.cpp` | Modify (update expected prompt strings) |

## Not changing

- Tool registration flow (`register.cpp`) — structure is fine
- Skills prompt system — already well-structured
- Memory injection in `build_system_prompt` — format is good, just needs more context around it
- Provider layer — no prompt changes needed there
- Hook system — hooks don't modify prompts, just observe
