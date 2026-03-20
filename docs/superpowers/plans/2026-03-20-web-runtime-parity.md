# Web Runtime Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use @superpowers:subagent-driven-development (recommended) or @superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the web chat runtime up to parity with CLI and channel runtimes for memory, tool/runtime context, skills, hooks, and approval flow, while preserving web-specific transport and read-only channel-session behavior.

**Architecture:** Introduce a shared runtime bootstrap under `src/app/runtime/` that builds the common agent execution bundle used by CLI, channel, and web. Rewire existing runtimes onto that bootstrap, then add a web-specific approval coordinator that adapts the existing synchronous tool approval callback to SSE plus a browser approval endpoint.

**Tech Stack:** C++23, SQLite, cpp-httplib, GoogleTest, React 19, TypeScript, React Router v7, Tailwind CSS v4, pnpm/Vite

---

## File Structure

### Shared Runtime Bootstrap

- Create: `src/app/runtime/agent-runtime.hpp`
  Defines the shared runtime bundle, builder inputs, and helper APIs for common runtime assembly.
- Create: `src/app/runtime/agent-runtime.cpp`
  Implements provider creation, runtime memory setup, tool context setup, runtime tool registration, skills prompt loading, hook loading, and `AgentLoop` construction.
- Modify: `CMakeLists.txt`
  Registers the new runtime source file and test file with the existing library and test targets.
- Test: `tests/app/runtime-agent-runtime-test.cpp`
  Verifies the shared builder produces the expected runtime capabilities and prompt composition.

### Existing Runtime Call Sites

- Modify: `src/app/bootstrap.cpp`
  Moves CLI runtime assembly and `--web` / `--web-only` server dependency injection onto the shared bootstrap path.
- Modify: `src/app/channel-serve.cpp`
  Replaces the in-file channel runtime assembly with the shared bootstrap while preserving channel-only fields and behavior.
- Modify: `src/app/channel-serve.hpp`
  Keeps runtime-facing declarations aligned if helper signatures or owned runtime fields change.
- Test: `tests/app/bootstrap-test.cpp`
  Covers CLI/web bootstrap dependency wiring.
- Test: `tests/app/channel-serve-test.cpp`
  Covers channel runtime parity after the bootstrap migration.

### Web Backend

- Modify: `src/features/web/web-types.hpp`
  Expands `WebSessionState` to hold the shared runtime bundle and pending approval state.
- Modify: `src/features/web/web-routes.hpp`
  Declares runtime-building helpers and the new approval route handler.
- Modify: `src/features/web/web-routes.cpp`
  Builds web chat sessions from the shared runtime bundle, emits approval SSE events, and resolves approval submissions.
- Modify: `src/features/web/web-server.hpp`
  Adds setter methods and stored dependencies needed to build parity-complete web runtimes.
- Modify: `src/features/web/web-server.cpp`
  Wires the new dependency setters and `POST /api/chat/approval` route.
- Test: `tests/features/web-routes-test.cpp`
  Covers route-level web runtime parity and approval flow.
- Test: `tests/features/web-chat-test.cpp`
  Covers chat event flow, memory/tool wiring, and approval-triggered chat behavior.

### Web Frontend

- Modify: `web/src/api/client.ts`
  Adds approval-submit helpers and new stream event typing for `approval_request`.
- Modify: `web/src/components/chat/ChatView.tsx`
  Tracks pending approval state, handles `approval_request`, calls the approval API, and preserves existing queue/abort behavior.
- Create: `web/src/components/chat/ApprovalPrompt.tsx`
  Focused UI for showing the approval prompt, command preview, sandbox mode, and approve/deny actions.
- Modify: `web/src/components/chat/types.ts`
  Adds frontend types for approval request payloads and related runtime event state.
- Test/Verify: `pnpm --dir web build`
  Ensures the web app still compiles after the new event and UI state wiring.

## Task 1: Build The Shared Agent Runtime Bootstrap

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/app/runtime/agent-runtime.hpp`
- Create: `src/app/runtime/agent-runtime.cpp`
- Test: `tests/app/runtime-agent-runtime-test.cpp`

- [ ] **Step 1: Write failing shared-bootstrap tests**

Add tests in `tests/app/runtime-agent-runtime-test.cpp` that build a runtime from a minimal fake agent config and assert:

```cpp
const auto runtime = build_agent_runtime(input);
EXPECT_TRUE(runtime.agent != nullptr);
EXPECT_TRUE(runtime.provider != nullptr);
EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "memory_list"));
EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "shell"));
EXPECT_NE(runtime.system_prompt.find("subagent"), std::string::npos);
```

Cover these behaviors:
- runtime memory exists when a memory store is provided,
- memory tools are registered,
- `append_subagent_prompt_guidance(...)` is reflected in the built prompt,
- skills prompt content is loaded when a skill directory is supplied,
- hook manager is created and loaded from resolved hook directories.

- [ ] **Step 2: Run the new runtime-bootstrap tests and verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "RuntimeAgentRuntimeTest"
```

Expected: FAIL because the shared runtime builder does not exist yet.

- [ ] **Step 3: Implement the shared runtime bundle and builder**

Create `src/app/runtime/agent-runtime.hpp` and `src/app/runtime/agent-runtime.cpp` with a focused API like:

```cpp
struct AgentRuntimeBundle {
    std::unique_ptr<Provider> provider;
    std::unique_ptr<RuntimeMemory> memory;
    ToolRegistry tools;
    ToolRuntimeContext tool_context;
    std::unique_ptr<McpManager> mcp_manager;
    std::string system_prompt;
    std::string skills_prompt;
    std::unique_ptr<HookManager> hook_manager;
    std::unique_ptr<AgentLoop> agent;
};

AgentRuntimeBundle build_agent_runtime(const AgentRuntimeBuildInput &input);
```

Implementation requirements:
- add the new runtime source and test file to `CMakeLists.txt`,
- resolve provider using the existing provider factory,
- build runtime memory from `RuntimeIdentity` and `Config::MemoryConfig`,
- populate the common `ToolRuntimeContext` fields,
- call `register_runtime_tools(...)`,
- load skills from `resolve_skill_directories(...)`,
- load hooks using existing hook directory rules,
- build `AgentLoop` with the resolved system prompt, runtime memory, skills prompt, and hook manager.

- [ ] **Step 4: Re-run the shared-bootstrap tests and verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "RuntimeAgentRuntimeTest"
```

Expected: PASS with the new runtime bundle covering memory, tools, prompt composition, skills, and hooks.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/app/runtime/agent-runtime.hpp src/app/runtime/agent-runtime.cpp tests/app/runtime-agent-runtime-test.cpp
git commit -m "feat: add shared agent runtime bootstrap"
```

## Task 2: Move CLI And Channel Onto The Shared Bootstrap

**Files:**
- Modify: `src/app/bootstrap.cpp`
- Modify: `src/app/channel-serve.cpp`
- Modify: `src/app/channel-serve.hpp`
- Test: `tests/app/bootstrap-test.cpp`
- Test: `tests/app/channel-serve-test.cpp`

- [ ] **Step 1: Write failing migration tests for CLI and channel parity**

Extend `tests/app/bootstrap-test.cpp` and `tests/app/channel-serve-test.cpp` to assert that the runtime call sites still expose the same capabilities after migration:

```cpp
EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "memory_list"));
EXPECT_EQ(runtime.tool_context.runtime_origin, SubagentRuntimeOrigin::channel);
EXPECT_EQ(runtime.tool_context.raw_caller_id, "qqbot:c2c:alice");
EXPECT_TRUE(runtime.agent != nullptr);
```

Also cover bootstrap-side wiring for web startup dependencies that will be needed later:
- `--web-only` creates the stores/managers required by web runtime assembly,
- `--web` attaches the same dependencies to the embedded web server instance.

- [ ] **Step 2: Run the CLI/channel tests and verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "BootstrapTest|ChannelServeTest"
```

Expected: FAIL because the call sites still hand-assemble their runtime state.

- [ ] **Step 3: Rewire CLI and channel assembly to use the shared builder**

Update `src/app/bootstrap.cpp` and `src/app/channel-serve.cpp` so they:
- construct `AgentRuntimeBuildInput`,
- call `build_agent_runtime(...)`,
- preserve existing origin-specific fields such as session scope, caller id, and channel-only extras,
- keep current behavior for single-message mode, REPL, and channel runtime persistence.

Do not widen scope here. The goal is parity-preserving migration, not a runtime redesign.

- [ ] **Step 4: Re-run the CLI/channel tests and verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "BootstrapTest|ChannelServeTest"
```

Expected: PASS with CLI/channel behavior unchanged except for now coming from the shared bootstrap.

- [ ] **Step 5: Commit**

```bash
git add src/app/bootstrap.cpp src/app/channel-serve.cpp src/app/channel-serve.hpp tests/app/bootstrap-test.cpp tests/app/channel-serve-test.cpp
git commit -m "refactor: migrate cli and channel runtimes to shared bootstrap"
```

## Task 3: Move Web Chat Onto The Shared Runtime Bundle

**Files:**
- Modify: `src/app/bootstrap.cpp`
- Modify: `src/features/web/web-server.hpp`
- Modify: `src/features/web/web-server.cpp`
- Modify: `src/features/web/web-types.hpp`
- Modify: `src/features/web/web-routes.hpp`
- Modify: `src/features/web/web-routes.cpp`
- Test: `tests/features/web-routes-test.cpp`
- Test: `tests/features/web-chat-test.cpp`

- [ ] **Step 1: Write failing web parity tests**

Add or extend web tests to assert:

```cpp
EXPECT_TRUE(has_tool_named(runtime.tools->definitions(), "memory_list"));
EXPECT_TRUE(has_tool_named(runtime.tools->definitions(), "custom_tool_name"));
EXPECT_EQ(runtime.tool_context.runtime_origin, SubagentRuntimeOrigin::web);
EXPECT_EQ(runtime.tool_context.raw_caller_id, "web:local");
EXPECT_EQ(runtime.tool_context.current_session_id, &runtime.session_id);
EXPECT_EQ(runtime.tool_context.allowed_child_agents, std::vector<std::string>({"coder"}));
EXPECT_TRUE(static_cast<bool>(runtime.tool_context.approval_callback));
EXPECT_TRUE(runtime.agent != nullptr);
```

Also cover user-visible web behavior:
- web chat can build a runtime when a memory store exists,
- web chat no longer reports missing memory tools,
- web chat uses permission-aware registration and execution guards, for example:

```cpp
const auto shell_result = runtime.tools->execute(ToolUseBlock{
    .id = "web-shell",
    .name = "shell",
    .input = {{"command", "echo hello"}},
});
EXPECT_TRUE(shell_result.is_error);
EXPECT_NE(shell_result.content.find("requires approval"), std::string::npos);
```

- web chat loads skills and hooks at the integration point, for example by using a temp skill/hook directory and asserting a non-empty skills prompt or a hook side effect during a tool call,
- web chat also preserves custom script tool and MCP registration from the shared runtime bootstrap, using a temp script-tool config and a test MCP server fixture if available,
- web chat keeps channel sessions read-only,
- web route setup includes the dependencies needed for shared runtime building.

- [ ] **Step 2: Run the web backend tests and verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: FAIL because web still creates a reduced runtime directly in `web-routes.cpp`.

- [ ] **Step 3: Inject shared runtime dependencies into WebServer and build web sessions from the shared bundle**

Update `src/features/web/web-server.hpp/.cpp`, `src/app/bootstrap.cpp`, and `src/features/web/web-routes.hpp/.cpp` so web runtime creation has access to:
- `Config`,
- `SessionStore`,
- `MemoryStore`,
- `SubagentManager`,
- any shared loader/config inputs needed by the new builder.

Replace web’s direct `Provider`/`ToolRegistry`/`AgentLoop` assembly with the shared runtime bundle. Preserve:
- SSE streaming,
- abort handling,
- session persistence,
- read-only rejection for channel sessions.

- [ ] **Step 4: Re-run the web backend tests and verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: PASS with web now exposing memory-aware, context-aware runtime behavior.

- [ ] **Step 5: Commit**

```bash
git add src/app/bootstrap.cpp src/features/web/web-server.hpp src/features/web/web-server.cpp src/features/web/web-types.hpp src/features/web/web-routes.hpp src/features/web/web-routes.cpp tests/features/web-routes-test.cpp tests/features/web-chat-test.cpp
git commit -m "feat: move web chat to shared runtime bootstrap"
```

## Task 4: Add Web Approval Coordination And API

**Files:**
- Modify: `src/features/web/web-types.hpp`
- Modify: `src/features/web/web-routes.hpp`
- Modify: `src/features/web/web-routes.cpp`
- Modify: `src/features/web/web-server.cpp`
- Test: `tests/features/web-routes-test.cpp`
- Test: `tests/features/web-chat-test.cpp`

- [ ] **Step 1: Write failing approval-flow tests**

Add tests that simulate a web runtime with `shell_approval = ask` and assert:

```cpp
EXPECT_EQ(first_event.name, "approval_request");
EXPECT_EQ(first_event.payload["tool"], "shell");
EXPECT_EQ(first_event.payload["sandbox_mode"], "isolated");
```

Also cover:
- approving the request lets the tool call run,
- denying the request returns a tool error,
- aborting the session cancels a pending approval,
- shutting down or cleaning up the active web session cancels a pending approval,
- approval requests expire after two minutes and behave as denied.

- [ ] **Step 2: Run the approval tests and verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: FAIL because web emits no approval event and has no approval endpoint.

- [ ] **Step 3: Implement pending-approval state, SSE emission, and `POST /api/chat/approval`**

Extend `WebSessionState` with a pending approval record and add a backend flow that:
- creates one pending approval at a time,
- emits `approval_request` on the active SSE stream,
- blocks the approval callback until resolved, cancelled, or timed out,
- exposes `POST /api/chat/approval` for resolving the pending request,
- cancels any unresolved request on abort, disconnect, or session cleanup.

Lock the API contract down in the implementation and tests:

Request body:

```json
{
  "session_id": "web-session-id",
  "request_id": "approval-1",
  "approved": true
}
```

Matching and validation rules:
- all three fields are required,
- `session_id` must refer to an active web session,
- `request_id` must match the currently pending approval on that session,
- a request can be resolved exactly once,
- if the session is missing, the approval is missing, or the `request_id` does not match, return an error instead of silently ignoring it.

Success responses:

```json
{"status":"approved"}
```

or

```json
{"status":"denied"}
```

Error responses:

```json
{"error":"missing or invalid 'session_id' field"}
```

```json
{"error":"missing or invalid 'request_id' field"}
```

```json
{"error":"missing or invalid 'approved' field"}
```

```json
{"error":"session not found"}
```

```json
{"error":"approval not found"}
```

```json
{"error":"approval already resolved"}
```

- [ ] **Step 4: Re-run the approval tests and verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: PASS with `approval_request` and approval resolution behavior covered.

- [ ] **Step 5: Commit**

```bash
git add src/features/web/web-types.hpp src/features/web/web-routes.hpp src/features/web/web-routes.cpp src/features/web/web-server.cpp tests/features/web-routes-test.cpp tests/features/web-chat-test.cpp
git commit -m "feat: add web approval coordination"
```

## Task 5: Add Web Approval UI And Final Regression Coverage

**Files:**
- Modify: `web/src/api/client.ts`
- Modify: `web/src/components/chat/ChatView.tsx`
- Create: `web/src/components/chat/ApprovalPrompt.tsx`
- Modify: `web/src/components/chat/types.ts`
- Test/Verify: `pnpm --dir web build`
- Verify: backend tests from prior tasks

- [ ] **Step 1: Add frontend event typing and failing UI handling checks**

Update `web/src/components/chat/types.ts` and `web/src/api/client.ts` so the web client understands an event payload shape like:

```ts
export type ApprovalRequest = {
  request_id: string
  tool: string
  command?: string
  sandbox_mode: string
  prompt: string
}
```

Then update `web/src/components/chat/ChatView.tsx` so the stream switch handles `approval_request` and the render path references an `ApprovalPrompt` component and `pendingApproval` state that do not exist yet. This is the deliberate red step for the frontend task.

- [ ] **Step 2: Run the frontend build and verify it fails or is incomplete without the approval UI**

Run:

```bash
pnpm --dir web build
```

Expected: FAIL due to the missing `ApprovalPrompt` component and missing approval state wiring in `ChatView.tsx`.

- [ ] **Step 3: Implement the approval UI and request handling**

Create `web/src/components/chat/ApprovalPrompt.tsx` and wire it into `ChatView.tsx` so the frontend:
- captures `approval_request` events,
- renders the pending approval near the active conversation,
- shows tool, command preview, sandbox mode, and prompt text,
- submits approve/deny decisions to `POST /api/chat/approval`,
- clears the pending approval state on resolution, abort, or stream completion,
- leaves existing queued-input behavior intact while the active request remains open.

- [ ] **Step 4: Run final regression checks**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "RuntimeAgentRuntimeTest|BootstrapTest|ChannelServeTest|WebRoutesTest|WebChatTest"
pnpm --dir web build
```

Expected: PASS across backend runtime tests and frontend build.

- [ ] **Step 5: Commit**

```bash
git add web/src/api/client.ts web/src/components/chat/ChatView.tsx web/src/components/chat/ApprovalPrompt.tsx web/src/components/chat/types.ts
git commit -m "feat: add web approval ui for runtime parity"
```
