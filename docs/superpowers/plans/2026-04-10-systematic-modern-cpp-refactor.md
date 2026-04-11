# Systematic Modern C++ Refactor Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Systematically refactor Orangutan to remove redundant code, unify reusable text/path utilities, adopt semantically correct modern C++23 interfaces, and split oversized modules by stable responsibilities without preserving obsolete signatures.

**Architecture:** Build shared `src/utils/` primitives first, then migrate tool/runtime surfaces to path-first and `std::string_view`-friendly interfaces, then delete duplicated text and escaping helpers across bootstrap/config/coordinator/skills, and only then decompose the largest runtime/channel modules. Keep generic cross-module helpers in `src/utils/`; keep repeated but domain-specific orchestration logic in the owning module instead of forcing fake utilities.

**Tech Stack:** C++23, STL/ranges, std::filesystem, nlohmann_json, magic_enum, ctre, spdlog, Catch2, xmake

**Scope note:** This plan supersedes the earlier same-day cleanup/refactor drafts for this topic. Execute this plan instead of stacking more partial cleanup waves on top of the older plan files.

---

## Planned File Structure

### New Files

- `src/utils/path.hpp` - generic path normalization, home expansion, containment checks, and workspace-relative resolution primitives.
- `src/utils/escape.hpp` - shared XML escaping and shell single-quote escaping helpers.
- `tests/utils/string-test.cpp` - coverage for trim, ASCII lowercase, enum-token normalization, and CSV splitting.
- `tests/utils/path-test.cpp` - coverage for normalize/expand/containment/path resolution helpers.
- `tests/utils/escape-test.cpp` - coverage for XML and shell escaping compatibility.
- `src/agent/agent-loop-history.hpp` - extracted history checkpointing, continuation, and compaction helpers kept header-defined per project guidance.
- `src/agent/agent-loop-tools.hpp` - extracted loop-detection and tool-execution helpers.
- `src/agent/agent-loop-memory.hpp` - extracted session-memory distillation parsing helpers.
- `src/bootstrap/channel-serve-approval.hpp` - channel approval prompt formatting/parsing helpers.
- `src/bootstrap/channel-serve-runtime.hpp` - conversation runtime inspection and session/runtime helper extraction.
- `src/bootstrap/channel-serve-delivery.hpp` - reply delivery helpers and outbound shaping.
- `src/channel/qq/qq-channel-outbound.hpp` - outbound payload routing, chunking, and media-send helpers.
- `src/channel/qq/qq-channel-inbound.hpp` - inbound parsing and message/reaction extraction helpers.
- `src/channel/qq/qq-channel-session.hpp` - session persistence, known-user persistence, and ref-index helpers.
- `src/channel/qq/qq-channel-runtime.hpp` - debounce/typing/runtime-thread helpers.

### Modified Files

- `src/utils/string.hpp`
- `src/utils/file-io.hpp`
- `src/utils/file-io.cpp`
- `src/tools/internal.hpp`
- `src/tools/register.hpp`
- `src/tools/register.cpp`
- `src/tools/runtime-loader/runtime-loader.hpp`
- `src/tools/runtime-loader/runtime-loader.cpp`
- `src/tools/shell/register.hpp`
- `src/tools/shell/register.cpp`
- `src/tools/shell/command-sandbox.hpp`
- `src/tools/shell/command-sandbox.cpp`
- `src/tools/shell/shell-tool.cpp`
- `src/tools/script/script-loader.hpp`
- `src/tools/script/script-loader.cpp`
- `src/tools/message-attachments/message-attachments-tool.hpp`
- `src/tools/message-attachments/message-attachments-tool.cpp`
- `src/tools/file-read/file-read-tool.cpp`
- `src/tools/registry/tool-registry.hpp`
- `src/tools/registry/tool-registry.cpp`
- `src/tools/registry/op-tool-support.hpp`
- `src/tools/automation/automation-tool-support.hpp`
- `src/tools/automation/automation-tool-support.cpp`
- `src/tools/task/task-tool.cpp`
- `src/tools/heartbeat/heartbeat-tool.cpp`
- `src/tools/inbox/inbox-tool.cpp`
- `src/bootstrap/cli-options.cpp`
- `src/bootstrap/bootstrap.cpp`
- `src/bootstrap/channel-serve.hpp`
- `src/bootstrap/channel-serve.cpp`
- `src/config/config-sections-core.cpp`
- `src/coordinator/agent-definition-registry.hpp`
- `src/coordinator/agent-definition-registry.cpp`
- `src/coordinator/coordinator-manager.cpp`
- `src/skills/skill-loader.hpp`
- `src/skills/skill-loader.cpp`
- `src/swarm/mailbox.hpp`
- `src/swarm/mailbox.cpp`
- `src/swarm/team-manager.hpp`
- `src/swarm/team-manager.cpp`
- `src/channel/channel.hpp`
- `src/channel/channel.cpp`
- `src/channel/qq/qq-message-builder.hpp`
- `src/channel/qq/qq-channel.hpp`
- `src/channel/qq/qq-channel.cpp`
- `src/agent/agent-loop.hpp`
- `src/agent/agent-loop.cpp`
- `tests/tools/registry/tool-registry-test.cpp`
- `tests/tools/script/script-tool-test.cpp`
- `tests/tools/message-attachments/message-attachments-tool-test.cpp`
- `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- `tests/tools/task/task-tool-test.cpp`
- `tests/tools/inbox/inbox-tool-test.cpp`
- `tests/bootstrap/runtime-agent-runtime-test.cpp`
- `tests/bootstrap/cli-options-test.cpp`
- `tests/bootstrap/channel-serve-test.cpp`
- `tests/config/config-test.cpp`
- `tests/coordinator/agent-definition-registry-test.cpp`
- `tests/coordinator/coordinator-manager-test.cpp`
- `tests/skills/skill-loader-test.cpp`
- `tests/swarm/mailbox-test.cpp`
- `tests/swarm/team-manager-test.cpp`
- `tests/channel/channel-test.cpp`
- `tests/channel/qq/qq-message-builder-test.cpp`
- `tests/channel/qq/qq-channel-test.cpp`
- `tests/channel/qq/qq-approval-keyboard-test.cpp`
- `tests/agent/agent-loop-test.cpp`

---

## Chunk 1: Shared Foundations

### Task 1: Lock Baseline Behavior For Shared Utility Extraction

**Files:**
- Reference: `AGENT.md`
- Reference: `docs/superpowers/plans/2026-04-10-systematic-modern-cpp-refactor.md`
- Modify: `tests/bootstrap/cli-options-test.cpp`
- Modify: `tests/coordinator/agent-definition-registry-test.cpp`
- Modify: `tests/tools/script/script-tool-test.cpp`

- [ ] **Step 1: Extend the existing production-facing suites so current normalization, CSV parsing, and shell-quoting behavior is locked before any shared utility API exists**

```cpp
TEST_CASE("cli permission mode accepts dashed and underscored spellings") {
    // exercise current cli-options behavior through the public option parsing path
}

TEST_CASE("agent definition frontmatter csv fields trim whitespace and skip blanks") {
    // exercise current registry file parsing behavior without referencing future utils APIs
}

TEST_CASE("script substitution preserves current shell quoting for apostrophes") {
    // exercise current script tool substitution behavior through the existing tool surface
}
```

- [ ] **Step 2: Run the existing production-facing suites to establish a green behavioral baseline before introducing any new utility headers or tests**

Run: `xmake build test-bootstrap test-coordinator test-tools && xmake run test-bootstrap && xmake run test-coordinator && xmake run test-tools`
Expected: PASS.

- [ ] **Step 3: Commit the baseline safety net**

```bash
git add tests/bootstrap/cli-options-test.cpp tests/coordinator/agent-definition-registry-test.cpp tests/tools/script/script-tool-test.cpp
git commit -m "test: lock shared utility behavior before refactor"
```

### Task 2: Build Shared `utils` Primitives And Remove Trivial I/O Duplication

**Files:**
- Modify: `src/utils/string.hpp`
- Create: `src/utils/path.hpp`
- Create: `src/utils/escape.hpp`
- Modify: `src/utils/file-io.hpp`
- Modify: `src/utils/file-io.cpp`
- Create: `tests/utils/string-test.cpp`
- Create: `tests/utils/path-test.cpp`
- Create: `tests/utils/escape-test.cpp`

- [ ] **Step 1: Create the dedicated utility tests and minimal header scaffolding for the new shared APIs so `test-utils` can fail in a controlled way instead of at missing-file compile time**

The scaffolding phase should create:
- `tests/utils/string-test.cpp`
- `tests/utils/path-test.cpp`
- `tests/utils/escape-test.cpp`
- `src/utils/path.hpp`
- `src/utils/escape.hpp`

with declarations or temporary placeholder bodies sufficient for the target to build.

- [ ] **Step 2: Extend `src/utils/string.hpp` with the reusable text helpers already duplicated across the codebase**

```cpp
[[nodiscard]] inline std::string ascii_to_lower_copy(std::string_view value);
[[nodiscard]] inline std::string normalize_enum_token(std::string_view value);
[[nodiscard]] inline std::vector<std::string> split_csv_trimmed(std::string_view value);
```

- [ ] **Step 3: Add `src/utils/path.hpp` for generic path logic now trapped in `tools/internal.hpp`, but keep security-sensitive sandbox policy outside `utils`**

```cpp
[[nodiscard]] inline std::filesystem::path normalize_path(const std::filesystem::path &path);
[[nodiscard]] inline std::filesystem::path expand_home_path(const std::filesystem::path &path);
[[nodiscard]] inline bool path_has_prefix(const std::filesystem::path &path, const std::filesystem::path &root);
[[nodiscard]] inline std::filesystem::path resolve_relative_to(const std::filesystem::path &path, const std::filesystem::path &root);
```

Required semantics:
- `normalize_path` uses canonical-style normalization when available and lexical normalization fallback when the target does not exist.
- `path_has_prefix` is a generic normalized-prefix helper, not the place for tool/workspace policy.
- security-sensitive callers must still expand home paths and normalize absolute operands before containment checks.

- [ ] **Step 4: Add `src/utils/escape.hpp` for cross-module escaping helpers**

```cpp
[[nodiscard]] inline std::string shell_single_quote_escape(std::string_view value);
[[nodiscard]] inline std::string escape_xml(std::string_view text);
```

- [ ] **Step 5: Run the new utility tests before finishing the real implementations so failures point at behavior rather than missing files**

Run: `xmake build test-utils && xmake run test-utils`
Expected: FAIL in the new utility assertions because placeholder or incomplete helper bodies are still in place.

- [ ] **Step 6: Collapse `write_file` and `write_file_binary` onto one tiny internal helper instead of keeping two open-coded `fwrite` blocks**

```cpp
namespace {
    void write_file_impl(const std::filesystem::path &path, std::string_view content, std::string_view mode);
}
```

- [ ] **Step 7: Re-run only the utility-focused suites before touching call sites elsewhere**

Run: `xmake build test-utils && xmake run test-utils`
Expected: PASS.

- [ ] **Step 8: Commit the shared primitives**

```bash
git add src/utils/string.hpp src/utils/path.hpp src/utils/escape.hpp src/utils/file-io.hpp src/utils/file-io.cpp tests/utils/string-test.cpp tests/utils/path-test.cpp tests/utils/escape-test.cpp
git commit -m "refactor: add shared string path and escape utilities"
```

---

## Chunk 2: Path-First Tool Surface

### Task 3: Move Tool Path Policy Onto Shared Path Utilities

**Files:**
- Modify: `src/tools/internal.hpp`
- Test: `tests/tools/registry/tool-registry-test.cpp`
- Test: `tests/tools/script/script-tool-test.cpp`
- Test: `tests/tools/message-attachments/message-attachments-tool-test.cpp`

- [ ] **Step 1: Replace local path normalization and home-expansion helpers in `src/tools/internal.hpp` with thin wrappers around `src/utils/path.hpp`**

Keep only tool-policy-specific functions here:
- permission-root expansion
- workspace sandbox validation
- tool working-dir resolution

- [ ] **Step 2: Remove any helper in `src/tools/internal.hpp` whose only job is generic normalization or prefix checking**

The target state is:
- generic path behavior in `orangutan::utils`
- tool sandbox policy in `orangutan::tools`

- [ ] **Step 3: Re-run the tool suites that exercise workspace path handling**

Run: `xmake build test-tools && xmake run test-tools`
Expected: PASS, especially the cases covering workspace-relative reads/writes, message attachment downloads, and script registration behavior.

- [ ] **Step 4: Commit the internal path-policy cleanup**

```bash
git add src/tools/internal.hpp tests/tools/registry/tool-registry-test.cpp tests/tools/script/script-tool-test.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp
git commit -m "refactor: separate generic path helpers from tool policy"
```

### Task 4: Convert Tool Registration And Runtime Bootstrap To Path-First Interfaces

**Files:**
- Modify: `src/tools/register.hpp`
- Modify: `src/tools/register.cpp`
- Modify: `src/tools/runtime-loader/runtime-loader.hpp`
- Modify: `src/tools/runtime-loader/runtime-loader.cpp`
- Modify: `src/tools/script/script-loader.hpp`
- Modify: `src/tools/script/script-loader.cpp`
- Modify: `src/tools/shell/register.hpp`
- Modify: `src/tools/shell/register.cpp`
- Modify: `src/tools/message-attachments/message-attachments-tool.hpp`
- Modify: `src/tools/message-attachments/message-attachments-tool.cpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `tests/bootstrap/runtime-agent-runtime-test.cpp`
- Modify: `tests/tools/message-attachments/message-attachments-tool-test.cpp`
- Modify: `tests/tools/script/script-tool-test.cpp`

- [ ] **Step 1: Change every workspace-like parameter in the builtin/runtime registration surface, including `register_script_tools`, from `const std::string &` to `const std::filesystem::path &`**

```cpp
void register_builtin_core_tools(ToolRegistry &registry,
                                 const std::filesystem::path &workspace_root = {},
                                 const ToolRuntimeContext *tool_context = nullptr,
                                 const ToolPermissionContext *permissions = nullptr,
                                 std::string_view edit_mode = "search_replace");

RuntimeToolBootstrapResult register_runtime_tools(...,
                                                  const std::filesystem::path &workspace_root,
                                                  ...);

void register_script_tools(ToolRegistry &registry,
                           const std::vector<ScriptToolConfig> &tools,
                           const std::filesystem::path &workspace_root = {},
                           ...);
```

- [ ] **Step 2: Update the downstream registration calls so there is no repeated `workspace.empty() ? path{} : path(workspace)` conversion boilerplate anywhere on the builtin/runtime/script registration path**

- [ ] **Step 3: Keep only truly text-like parameters as `std::string_view`, such as `edit_mode`, while leaving owning config data as `std::string` or `std::filesystem::path`**

- [ ] **Step 4: In every deferred callback or stored lambda on this path, capture owning `std::string` or `std::filesystem::path` values instead of `std::string_view`; no new `std::string_view` may be stored in members, containers, or deferred captures**

- [ ] **Step 5: Rewrite the affected tests to use path-first helpers directly rather than preserving old string-based helper wrappers**

- [ ] **Step 6: Re-run runtime/bootstrap/tool registration coverage**

Run: `xmake build test-tools test-bootstrap && xmake run test-tools && xmake run test-bootstrap`
Expected: PASS.

- [ ] **Step 7: Commit the path-first registration surface**

```bash
git add src/tools/register.hpp src/tools/register.cpp src/tools/runtime-loader/runtime-loader.hpp src/tools/runtime-loader/runtime-loader.cpp src/tools/script/script-loader.hpp src/tools/script/script-loader.cpp src/tools/shell/register.hpp src/tools/shell/register.cpp src/tools/message-attachments/message-attachments-tool.hpp src/tools/message-attachments/message-attachments-tool.cpp tests/tools/registry/tool-registry-test.cpp tests/bootstrap/runtime-agent-runtime-test.cpp tests/tools/message-attachments/message-attachments-tool-test.cpp tests/tools/script/script-tool-test.cpp
git commit -m "refactor: make tool runtime registration path-first"
```

### Task 5: Refactor Script And Shell Plumbing Onto Shared Escape + Path Utilities

**Files:**
- Modify: `src/tools/script/script-loader.hpp`
- Modify: `src/tools/script/script-loader.cpp`
- Modify: `src/tools/shell/command-sandbox.hpp`
- Modify: `src/tools/shell/command-sandbox.cpp`
- Modify: `src/tools/shell/shell-tool.cpp`
- Test: `tests/tools/script/script-tool-test.cpp`
- Test: `tests/tools/shell/background-shell-completion-test.cpp`

- [ ] **Step 1: Replace the duplicated `shell_escape` implementations in script-loader and command-sandbox with `utils::shell_single_quote_escape`**

- [ ] **Step 2: Make sandbox workspace and working-directory parameters path-first, leaving only actual shell command text as string/view data**

```cpp
[[nodiscard]] SandboxedCommand prepare_sandboxed_command(std::string_view command,
                                                         const std::filesystem::path &workspace_root,
                                                         const std::filesystem::path &working_dir,
                                                         tool_sandbox_mode sandbox_mode);
```

- [ ] **Step 3: Remove string/path round-trips inside script execution so the code resolves directories once, then only formats strings at process-boundary APIs**

Security invariant for this task:
- preserve the current sandbox behavior for nonexistent paths by normalizing with canonical-or-lexical fallback before comparison.
- expand and normalize both workspace and candidate paths before containment checks.
- keep symlink-sensitive workspace enforcement in the tool-policy layer rather than weakening it into a raw lexical prefix test.

- [ ] **Step 4: Use the new shared utilities inside `src/tools/shell/shell-tool.cpp` for trim/token/path normalization where the semantics match, but keep command-token policy local to the shell tool**

- [ ] **Step 5: Re-run the shell/script suites**

Run: `xmake build test-tools && xmake run test-tools`
Expected: PASS, including script substitution, sandboxed shell execution, and background shell completion tests.

- [ ] **Step 6: Commit the shell/script cleanup**

```bash
git add src/tools/script/script-loader.hpp src/tools/script/script-loader.cpp src/tools/shell/command-sandbox.hpp src/tools/shell/command-sandbox.cpp src/tools/shell/shell-tool.cpp tests/tools/script/script-tool-test.cpp tests/tools/shell/background-shell-completion-test.cpp
git commit -m "refactor: unify shell and script path handling"
```

### Task 6: Collapse Repeated Tool-Orchestration Boilerplate Where It Is Domain-Specific

**Files:**
- Modify: `src/tools/registry/op-tool-support.hpp`
- Modify: `src/tools/automation/automation-tool-support.hpp`
- Modify: `src/tools/automation/automation-tool-support.cpp`
- Modify: `src/tools/task/task-tool.cpp`
- Modify: `src/tools/heartbeat/heartbeat-tool.cpp`
- Modify: `src/tools/inbox/inbox-tool.cpp`
- Modify: `src/tools/file-read/file-read-tool.cpp`
- Test: `tests/tools/task/task-tool-test.cpp`
- Test: `tests/tools/heartbeat/heartbeat-tool-test.cpp`
- Test: `tests/tools/inbox/inbox-tool-test.cpp`
- Test: `tests/tools/registry/tool-abstraction-helpers-test.cpp`

- [ ] **Step 1: Add one shared helper for the repeated default-agent-key / `id`-or-`name` resolution patterns in the automation tools**

```cpp
[[nodiscard]] std::string resolve_agent_key(const ToolRuntimeContext &ctx);
[[nodiscard]] std::string id_or_name(const nlohmann::json &request);
```

- [ ] **Step 2: Move repeated task/heartbeat/inbox operation-routing boilerplate into `src/tools/automation/automation-tool-support.*` instead of `src/utils/`, because the logic is tool-domain-specific rather than globally reusable**

- [ ] **Step 3: Reuse `utils::ascii_to_lower_copy` or `utils::normalize_enum_token` in `src/tools/file-read/file-read-tool.cpp` only where the current logic is truly generic text classification**

- [ ] **Step 4: Re-run the untagged shared tool suite after removing the boilerplate**

Run: `xmake build test-tools && xmake run test-tools`
Expected: PASS.

- [ ] **Step 5: Commit the domain-specific tool support cleanup**

```bash
git add src/tools/registry/op-tool-support.hpp src/tools/automation/automation-tool-support.hpp src/tools/automation/automation-tool-support.cpp src/tools/task/task-tool.cpp src/tools/heartbeat/heartbeat-tool.cpp src/tools/inbox/inbox-tool.cpp src/tools/file-read/file-read-tool.cpp tests/tools/task/task-tool-test.cpp tests/tools/heartbeat/heartbeat-tool-test.cpp tests/tools/inbox/inbox-tool-test.cpp tests/tools/registry/tool-abstraction-helpers-test.cpp
git commit -m "refactor: collapse repeated automation tool plumbing"
```

---

## Chunk 3: Semantic API Cleanup And Duplicate Text Logic Removal

### Task 7: Remove Repeated Text Helpers Across Bootstrap, Config, And Coordinator

**Files:**
- Modify: `src/bootstrap/cli-options.cpp`
- Modify: `src/config/config-sections-core.cpp`
- Modify: `src/coordinator/agent-definition-registry.hpp`
- Modify: `src/coordinator/agent-definition-registry.cpp`
- Modify: `src/bootstrap/bootstrap.cpp`
- Modify: `src/coordinator/coordinator-manager.cpp`
- Test: `tests/bootstrap/cli-options-test.cpp`
- Test: `tests/config/config-test.cpp`
- Test: `tests/coordinator/agent-definition-registry-test.cpp`
- Test: `tests/coordinator/coordinator-manager-test.cpp`

- [ ] **Step 1: Replace duplicated `normalize_enum_token`, `trim`, `split_csv`, `shell_escape`, and `escape_xml` implementations with the new shared helpers where the behavior is truly identical**

- [ ] **Step 2: Keep only the remaining domain-specific parsing helpers local to their module, and delete helpers that are now one-line wrappers around `utils`**

- [ ] **Step 3: Make `AgentDefinitionRegistry` directory/path entry points path-first if the parameter is semantically a filesystem path**

```cpp
void load_from_directory(const std::filesystem::path &directory_path);
std::optional<AgentDefinition> find(std::string_view key) const;
bool has(std::string_view key) const;
```

- [ ] **Step 4: Re-run the affected suites**

Run: `xmake build test-bootstrap test-config test-coordinator && xmake run test-bootstrap && xmake run test-config && xmake run test-coordinator`
Expected: PASS.

- [ ] **Step 5: Commit the duplicate text-helper cleanup**

```bash
git add src/bootstrap/cli-options.cpp src/config/config-sections-core.cpp src/coordinator/agent-definition-registry.hpp src/coordinator/agent-definition-registry.cpp src/bootstrap/bootstrap.cpp src/coordinator/coordinator-manager.cpp tests/bootstrap/cli-options-test.cpp tests/config/config-test.cpp tests/coordinator/agent-definition-registry-test.cpp tests/coordinator/coordinator-manager-test.cpp
git commit -m "refactor: remove duplicated text helpers across core modules"
```

### Task 8: Normalize String And Path Semantics In Mid-Sized Core Interfaces

**Files:**
- Modify: `src/skills/skill-loader.hpp`
- Modify: `src/skills/skill-loader.cpp`
- Modify: `src/swarm/mailbox.hpp`
- Modify: `src/swarm/mailbox.cpp`
- Modify: `src/swarm/team-manager.hpp`
- Modify: `src/swarm/team-manager.cpp`
- Modify: `src/channel/channel.hpp`
- Modify: `src/channel/channel.cpp`
- Modify: `src/channel/qq/qq-message-builder.hpp`
- Modify: `src/tools/registry/tool-registry.hpp`
- Modify: `src/tools/registry/tool-registry.cpp`
- Test: `tests/skills/skill-loader-test.cpp`
- Test: `tests/swarm/mailbox-test.cpp`
- Test: `tests/swarm/team-manager-test.cpp`
- Test: `tests/channel/channel-test.cpp`
- Test: `tests/channel/qq/qq-message-builder-test.cpp`
- Test: `tests/tools/registry/tool-registry-test.cpp`

- [ ] **Step 1: Make DB/config/workspace path parameters path-first in swarm and skill-loader entry points where the code semantically receives paths rather than generic text**

Examples:
- `AgentMailbox(const std::filesystem::path &db_path)`
- `TeamManager(const std::filesystem::path &db_path)`
- `SkillLoader::load_from_directories(const std::vector<std::filesystem::path> &directories)` or an equivalent path-based overload

- [ ] **Step 2: Change pure lookup/read-only interfaces to `std::string_view` when they do not store or mutate the input**

Examples:
- `SkillLoader::find_skill(std::string_view name) const`
- `ToolRegistry::find_definition(std::string_view name) const`
- `ToolRegistry::find_tool(std::string_view name) const`

- [ ] **Step 3: Update `src/channel/channel.hpp` convenience APIs and `src/channel/qq/qq-message-builder.hpp` setter-style methods to take `std::string_view` or path types when they immediately materialize owned output and do not retain borrowed storage**

```cpp
QqMessageBuilder &text(std::string_view content);
QqMessageBuilder &markdown(std::string_view content);
QqMessageBuilder &reply_to(std::string_view message_id);
Attachment download_attachment(std::string_view jid, const Attachment &attachment, const std::filesystem::path &destination_path) override;
```

- [ ] **Step 4: Re-run misc services, swarm, channel, and tool-registry coverage together to catch signature drift**

Run: `xmake build test-misc-services test-swarm test-channel test-tools && xmake run test-misc-services && xmake run test-swarm && xmake run test-channel && xmake run test-tools`
Expected: PASS.

- [ ] **Step 5: Commit the semantic interface cleanup**

```bash
git add src/skills/skill-loader.hpp src/skills/skill-loader.cpp src/swarm/mailbox.hpp src/swarm/mailbox.cpp src/swarm/team-manager.hpp src/swarm/team-manager.cpp src/channel/channel.hpp src/channel/channel.cpp src/channel/qq/qq-message-builder.hpp src/tools/registry/tool-registry.hpp src/tools/registry/tool-registry.cpp tests/skills/skill-loader-test.cpp tests/swarm/mailbox-test.cpp tests/swarm/team-manager-test.cpp tests/channel/channel-test.cpp tests/channel/qq/qq-message-builder-test.cpp tests/tools/registry/tool-registry-test.cpp
git commit -m "refactor: align string and path semantics in core interfaces"
```

---

## Chunk 4: Large Module Decomposition

### Task 9: Split `AgentLoop` By History, Tool Execution, And Session-Memory Responsibilities

**Files:**
- Create: `src/agent/agent-loop-history.hpp`
- Create: `src/agent/agent-loop-tools.hpp`
- Create: `src/agent/agent-loop-memory.hpp`
- Modify: `src/agent/agent-loop.hpp`
- Modify: `src/agent/agent-loop.cpp`
- Test: `tests/agent/agent-loop-test.cpp`

- [ ] **Step 1: Freeze current `AgentLoop` behavior with focused tests around continuation, loop detection, and distilled-session parsing before extracting helpers**

- [ ] **Step 2: Move free helper structs/functions out of the monolithic `.cpp` into focused internal headers, keeping declarations and definitions together per project guidance**

Suggested split:
- `agent-loop-history.hpp`: `emit_history_checkpoint`, continuation helpers, history compaction helpers
- `agent-loop-tools.hpp`: `ToolCallSignature`, hashing, loop detection, tool execution helpers
- `agent-loop-memory.hpp`: distilled-session parsing helpers and data structures

- [ ] **Step 3: Shrink `AgentLoop` private state to only persistent runtime state, deleting helper-specific nested types or member functions that no longer need class access**

- [ ] **Step 4: Re-run the agent suite before and after any follow-up cleanup of redundant private members/functions**

Run: `xmake build test-agent && xmake run test-agent`
Expected: PASS.

- [ ] **Step 5: Commit the `AgentLoop` decomposition**

```bash
git add src/agent/agent-loop-history.hpp src/agent/agent-loop-tools.hpp src/agent/agent-loop-memory.hpp src/agent/agent-loop.hpp src/agent/agent-loop.cpp tests/agent/agent-loop-test.cpp
git commit -m "refactor: split agent loop responsibilities"
```

### Task 10: Split `channel-serve` Into Approval, Runtime, And Delivery Units

**Files:**
- Create: `src/bootstrap/channel-serve-approval.hpp`
- Create: `src/bootstrap/channel-serve-runtime.hpp`
- Create: `src/bootstrap/channel-serve-delivery.hpp`
- Modify: `src/bootstrap/channel-serve.hpp`
- Modify: `src/bootstrap/channel-serve.cpp`
- Test: `tests/bootstrap/channel-serve-test.cpp`
- Test: `tests/channel/qq/qq-approval-keyboard-test.cpp`

- [ ] **Step 1: Add or extend tests for approval prompt formatting/parsing, reply delivery, and runtime inspection so behavior is pinned before any split**

- [ ] **Step 2: Extract approval-only helpers into `channel-serve-approval.hpp`**

This unit should own:
- request-id formatting/parsing helpers
- approval prompt text/keyboard shaping
- inbound approval reply parsing

- [ ] **Step 3: Extract conversation-runtime inspection and resume-callback construction into `channel-serve-runtime.hpp`, leaving the outer loop and cross-module orchestration in `channel-serve.cpp`**

- [ ] **Step 4: Extract outbound reply helpers into `channel-serve-delivery.hpp`, then delete redundant file-local helpers and any state that becomes derivable**

- [ ] **Step 5: Re-run channel/bootstrap suites after each extraction stage**

Run: `xmake build test-bootstrap test-channel && xmake run test-bootstrap && xmake run test-channel`
Expected: PASS.

- [ ] **Step 6: Commit the `channel-serve` decomposition**

```bash
git add src/bootstrap/channel-serve-approval.hpp src/bootstrap/channel-serve-runtime.hpp src/bootstrap/channel-serve-delivery.hpp src/bootstrap/channel-serve.hpp src/bootstrap/channel-serve.cpp tests/bootstrap/channel-serve-test.cpp tests/channel/qq/qq-approval-keyboard-test.cpp
git commit -m "refactor: split channel serve responsibilities"
```

### Task 11: Split `QqChannel` Into Outbound, Inbound, Session, And Runtime Units

**Files:**
- Create: `src/channel/qq/qq-channel-outbound.hpp`
- Create: `src/channel/qq/qq-channel-inbound.hpp`
- Create: `src/channel/qq/qq-channel-session.hpp`
- Create: `src/channel/qq/qq-channel-runtime.hpp`
- Modify: `src/channel/qq/qq-channel.hpp`
- Modify: `src/channel/qq/qq-channel.cpp`
- Test: `tests/channel/qq/qq-channel-test.cpp`
- Test: `tests/channel/qq/qq-message-builder-test.cpp`
- Test: `tests/channel/qq/qq-approval-keyboard-test.cpp`

- [ ] **Step 1: Lock current behavior for outbound chunking, media sending, ref-index lookup, inbound parsing, and debounce typing behavior before splitting code**

- [ ] **Step 2: Extract outbound-only helpers into `qq-channel-outbound.hpp`**

This unit should own:
- send-target resolution
- message/media endpoint path building
- text chunking and outbound payload routing
- media segment send helpers

- [ ] **Step 3: Extract inbound parsing helpers into `qq-channel-inbound.hpp`**

This unit should own:
- attachment extraction
- mention parsing and bot-mention checks
- scene metadata parsing
- reaction/inbound message shaping helpers

- [ ] **Step 4: Extract session and runtime support into `qq-channel-session.hpp` and `qq-channel-runtime.hpp`, then trim `QqChannel` private surface to persistent state and externally meaningful operations only**

Candidate reductions:
- demote helper-only private member functions into free functions
- group runtime-thread helpers away from business methods
- delete any helper members that become one-shot derivations after extraction

- [ ] **Step 5: Re-run the QQ/channel suites after every extraction stage**

Run: `xmake build test-channel && xmake run test-channel`
Expected: PASS.

- [ ] **Step 6: Commit the `QqChannel` decomposition**

```bash
git add src/channel/qq/qq-channel-outbound.hpp src/channel/qq/qq-channel-inbound.hpp src/channel/qq/qq-channel-session.hpp src/channel/qq/qq-channel-runtime.hpp src/channel/qq/qq-channel.hpp src/channel/qq/qq-channel.cpp tests/channel/qq/qq-channel-test.cpp tests/channel/qq/qq-message-builder-test.cpp tests/channel/qq/qq-approval-keyboard-test.cpp
git commit -m "refactor: split qq channel responsibilities"
```

### Task 12: Final Verification, Residual Cleanup, And Deferred Follow-Ups

**Files:**
- Modify: `docs/superpowers/plans/2026-04-10-systematic-modern-cpp-refactor.md`

- [x] **Step 1: Run the touched-area build matrix**

Run: `xmake build test-utils && xmake build test-tools && xmake build test-bootstrap && xmake build test-config && xmake build test-coordinator && xmake build test-misc-services && xmake build test-swarm && xmake build test-channel && xmake build test-agent`
Expected: build succeeds.

Result: passed on 2026-04-11 after rerunning the full matrix fresh following the Task 12 linker fix in `agent-loop`/`utils::format`.

- [x] **Step 2: Run the touched-area execution matrix**

Run: `xmake run test-utils && xmake run test-tools && xmake run test-bootstrap && xmake run test-config && xmake run test-coordinator && xmake run test-misc-services && xmake run test-swarm && xmake run test-channel && xmake run test-agent`
Expected: PASS.

Result: passed on 2026-04-11.
Observed passing results:
`test-utils`: 26 assertions in 15 test cases.
`test-tools`: 787 assertions in 248 test cases.
`test-bootstrap`: 444 assertions in 84 test cases.
`test-config`: 102 assertions in 15 test cases.
`test-coordinator`: 44 assertions in 8 test cases.
`test-misc-services`: 115 assertions in 43 test cases.
`test-swarm`: 13 assertions in 2 test cases.
`test-channel`: 262 assertions in 46 test cases.
`test-agent`: 135 assertions in 17 test cases.

- [ ] **Step 3: Run broader regression coverage if any touched-area failures required cross-module fixes**

Run: `xmake build test-integration test-process test-permissions && xmake run test-integration && xmake run test-process && xmake run test-permissions`
Expected: PASS, or document clearly why a broader rerun is deferred.

Result: broader coverage was executed because Task 12 initially required a cross-module follow-up in `src/agent/agent-loop.*` and `src/utils/format.hpp`, and that run exposed a reproducible `test-process` failure.
Root cause: `BackgroundProcessManager` could return from `start()` before a freshly forked background child had reset inherited test-process signal handlers, so an immediate `kill()` could deliver `SIGTERM` to the pre-`exec` child while it still carried Catch2's fatal signal handler.
Fix: `src/process/subprocess.cpp` now resets child signal state and uses a small readiness handshake before `BackgroundProcessManager::start()` returns.
Final broader rerun results on 2026-04-11:
`test-integration`: passed with 63 assertions in 11 test cases.
`test-process`: passed with 2145 assertions in 21 test cases.
`test-permissions`: passed with 93 assertions in 42 test cases.

- [x] **Step 4: Record residual follow-up only for work intentionally left outside this plan's scope**

Allowed residuals:
- provider-layer modernization unrelated to this refactor
- storage/web cleanup not touched by any wave above

Not allowed residuals:
- leaving duplicate helpers in the touched areas
- leaving path-as-string interfaces in the touched areas without justification

Result: no in-scope residual cleanup was left in the touched refactor areas.

- [ ] **Step 5: Commit the verification record and any deferred-work notes**

```bash
git add docs/superpowers/plans/2026-04-10-systematic-modern-cpp-refactor.md
git commit -m "docs: record systematic refactor verification status"
```

Status: this file was updated with the verification record. Commit status depends on the current session action.
