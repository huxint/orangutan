# Web Runtime Parity — Design Spec

**Date:** 2026-03-20
**Status:** Approved

## Overview

Bring the web chat runtime up to parity with the existing CLI and channel runtimes for agent execution capabilities. The web entry point should assemble the same core runtime pieces as other runtimes so that agent behavior stays consistent across surfaces.

This work is focused on runtime wiring, not on turning the web UI into a terminal emulator. The web UI keeps its own transport and presentation model, but it should no longer silently miss runtime capabilities that already exist elsewhere.

## Goals

- Make web chat use the same agent runtime building blocks as CLI and channel runtimes.
- Ensure web chat has working runtime memory, memory tools, runtime context, subagent context, custom tools, MCP tools, skills, hooks, and permission-aware tool registration.
- Add web approval handling for `shell_approval = ask` so approval works in browser sessions instead of failing with "interactive approval is unavailable".
- Keep web-specific behavior such as SSE streaming and read-only channel sessions.
- Reduce future drift so newly added runtime capabilities do not need to be manually re-wired into web chat one by one.

## Non-Goals

- No PTY or interactive terminal session support in this change.
- No redesign of shell sandbox semantics beyond making web respect the existing policy.
- No change to the read-only rule for channel-backed sessions in the web UI.
- No attempt to make web output look like a terminal.

## Problem Statement

The current web runtime path assembles `Provider`, `ToolRegistry`, and `AgentLoop` separately from the CLI and channel runtime paths. That drift already caused several bugs:

- web chat missed permission-aware tool registration and could bypass expected shell policy,
- web chat lacked runtime memory wiring, so memory tools and memory-backed prompt context were absent,
- web chat did not run the same skills and hooks path,
- web approval requests could not be satisfied because no browser-facing approval callback existed.

The issue is structural. As long as web constructs its own runtime by hand, future runtime additions will keep landing in CLI or channel first and web will keep lagging behind.

## Key Decisions

### Web Should Reuse a Shared Runtime Bootstrap

The fix should not be another series of point patches in `web-routes.cpp`. Instead, runtime assembly should be centralized into a shared builder that all runtime entry points can use.

That builder should produce one coherent runtime bundle containing the objects needed to run an agent turn. Each caller can still wrap that bundle in its own session container, but the construction of the runtime itself should be shared.

### Parity Covers Execution Capabilities, Not Presentation

Parity here means "the agent has the same execution capabilities and policy inputs". It does not mean the web UI should behave like a terminal.

What should be aligned:

- runtime memory and memory tools,
- workspace and permission policy,
- tool runtime context,
- subagent wiring,
- custom tools and MCP tools,
- skills prompt injection,
- hooks,
- approval callbacks.

What remains intentionally different:

- SSE transport,
- browser UI state,
- channel sessions shown as read-only in web,
- how command output is rendered to the user.

### Approval Flow Remains Synchronous at the Runtime Layer

The existing permission system expects a synchronous approval callback. Web support should adapt to that expectation rather than replacing it with a second permission system.

The browser path should therefore:

- emit an approval request event over SSE,
- block the runtime callback while waiting for a user decision,
- accept the decision over a dedicated HTTP endpoint,
- resume or reject the tool call.

This preserves one approval model across CLI, channel, subagent, and web runtimes.

## Architecture

### Shared Agent Runtime Bundle

Add a new shared runtime bootstrap layer under `src/app/` that builds the runtime objects needed for one agent execution context.

A representative bundle shape is:

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
```

Exact ownership details may vary, but the bundle must be capable of representing the full runtime needed by CLI, channel, and web.

The builder inputs should include:

- effective agent config,
- runtime identity,
- runtime origin,
- current session id pointer,
- `MemoryStore`,
- `SubagentManager`,
- custom tools and MCP config,
- approval callback,
- caller id metadata,
- a small runtime-specific context extension or pre-populated `ToolRuntimeContext` fields supplied by callers that need extras beyond the common base wiring.

### Web Session State Wraps the Shared Bundle

`WebSessionState` should continue to exist because web has its own transport lifecycle, abort state, and temporary session bookkeeping.

However, instead of owning ad hoc pieces built directly in `web-routes.cpp`, it should own the shared runtime bundle or the same object graph returned by the shared builder.

This keeps web-specific lifecycle state separate from shared runtime assembly.

### Runtime Memory Must Be Present in Web

Web chat should construct `RuntimeMemory` exactly as other runtimes do when a memory store is available. This enables two separate behaviors that are both required:

- `AgentLoop` can inject relevant memories into the system prompt context,
- memory tools such as `remember`, `recall`, and `memory_list` are registered into the tool set.

Without this, the agent cannot reliably answer questions like "what memories do you have" by consulting memory tools.

### Tool Runtime Context Must Be Present in Web

The web runtime must populate a real `ToolRuntimeContext`, including:

- `runtime_key`,
- `agent_key`,
- `scope_key`,
- `current_session_id`,
- `allowed_child_agents`,
- `is_child_run = false`,
- `subagent_manager`,
- `runtime_origin = web`,
- `raw_caller_id = "web:local"`,
- `approval_callback`.

This is required so that subagent tools, approval-aware tools, and future runtime-aware tools behave consistently in web chat.

### Web Uses Runtime Tool Registration, Not a Special Case

Web must call the same runtime tool bootstrap used by other runtimes:

- built-in tools,
- memory tools,
- custom script tools,
- permission filtering and execution guards,
- MCP tool registration.

Any direct built-in-only tool registration path in web should be removed or reduced to calling the shared bootstrap.

### Prompt Composition Must Match Other Runtimes

The final system prompt used in web should be assembled the same way as CLI and channel runtimes:

- agent system prompt,
- subagent guidance from `append_subagent_prompt_guidance(...)`,
- active skills prompt section,
- runtime memory contextual injection performed by `AgentLoop`.

This keeps agent behavior and instruction hierarchy aligned across entry points.

### Hooks Must Run in Web Chat

Web runtime should load and attach the same hook set used by other runtimes, using the same directory resolution rules.

At minimum, these hook events should be able to fire in web sessions exactly as they do elsewhere:

- `message_received`,
- `before_tool_call`,
- `after_tool_call`,
- `message_sending`,
- `session_start`,
- `session_end`.

## Web Approval Flow

### SSE Event

Add a new SSE event for pending approvals:

- `event: approval_request`

Payload should include:

- `request_id`,
- `tool`,
- `command` when applicable,
- `sandbox_mode`,
- `prompt`.

This gives the frontend enough information to render an approval card or modal without inventing a second prompt format.

### Approval Endpoint

Add a dedicated endpoint such as:

- `POST /api/chat/approval`

Request body:

```json
{
  "session_id": "...",
  "request_id": "...",
  "approved": true
}
```

The backend should validate that the pending approval exists for the given web session, resolve it exactly once, and return a small status payload.

### Approval Timeout

Web approvals should not wait forever. To match the existing approval behavior used elsewhere, a pending approval should expire after two minutes and be treated as denied.

This gives the implementation plan a concrete timeout target and avoids sessions hanging indefinitely on abandoned browser tabs.

### Pending Approval State

Each active `WebSessionState` should track at most one outstanding approval request at a time.

A representative state object:

```cpp
struct PendingApproval {
    std::string request_id;
    std::string prompt;
    std::mutex mutex;
    std::condition_variable cv;
    bool resolved = false;
    bool approved = false;
    bool cancelled = false;
};
```

When a tool approval is required:

1. create and store the pending approval,
2. emit `approval_request` over SSE,
3. block the callback until approved, denied, cancelled, or timed out,
4. clear the pending approval.

### Cancellation Rules

Pending approvals must be cancelled if any of the following happens:

- the user aborts the session,
- the SSE stream is torn down,
- the web session is cleaned up,
- the server shuts down.

A cancelled approval should behave like a rejected approval from the runtime's perspective.

### SSE Disconnect Semantics

This design does not introduce resumable in-flight web streams. If the browser disconnects during an active turn, the active web runtime should be treated as torn down for that request:

- pending approvals are cancelled,
- the in-flight turn stops,
- a later browser load can only resume from persisted session history, not from the previous live SSE stream state.

## API Changes

### `POST /api/chat`

No major request shape change is required for this design beyond existing agent-scoped chat inputs. The response stream gains one new event type:

- `approval_request`

The rest of the event model remains intact:

- `session`,
- `text`,
- `tool_start`,
- `tool_end`,
- `done`,
- `error`.

### `POST /api/chat/approval`

New endpoint to resolve a pending approval request for a web chat session.

Success cases:

- approval accepted,
- approval denied.

Error cases:

- missing or invalid fields,
- session not found,
- no matching pending approval,
- approval already resolved.

## Frontend Behavior

### Approval UI

When the frontend receives `approval_request`, it should present a focused approval UI near the active conversation.

The UI should show:

- tool name,
- command preview when available,
- sandbox mode,
- the approval prompt text,
- explicit `Approve` and `Deny` actions.

This UI is runtime state, not persisted chat history.

### Message Flow

The active assistant message remains the correct place to show tool calls and text deltas. Approval requests are separate runtime events and should not be persisted as assistant content blocks unless a deliberate product decision is made later.

### Read-Only Channel Sessions

The existing rule remains unchanged:

- channel sessions can be listed and viewed in web,
- channel sessions cannot be continued from web.

Runtime parity does not override that UX decision.

## Testing

### Backend Tests

Add or update tests that cover:

- web runtime registers memory tools when memory storage is available,
- web `AgentLoop` receives runtime memory and can therefore use memory-backed prompt context,
- web runtime receives a populated `ToolRuntimeContext` with `runtime_origin = web`,
- web runtime uses permission-aware tool registration,
- web runtime includes skills prompt and hook manager wiring,
- web approval requests are emitted when `shell_approval = ask`,
- approving a request allows the tool call to proceed,
- denying a request rejects the tool call,
- aborting or tearing down the session cancels a pending approval,
- read-only channel session restrictions still hold.

### Frontend Verification

Verify that:

- browser chat can surface approval requests and resolve them,
- a memory-related user request can trigger memory tool usage because those tools exist in the web runtime,
- the existing tool call rendering still works with the expanded runtime wiring,
- channel sessions remain viewable but read-only.

### Regression Guard

The most important regression guard is structural: web should no longer hand-assemble a reduced runtime. The tests should make it hard to accidentally remove runtime memory, approval wiring, or hook/skill integration from web in future refactors.

## Risks

- Approval handling adds blocking synchronization to the web path; incorrect cleanup could deadlock a session.
- Hook execution in web may expose previously hidden hook failures because those hooks were not always running before.
- Sharing runtime bootstrap code across CLI, channel, and web increases coupling, so the builder API should stay narrowly focused on runtime assembly.

## Implementation Notes

A pragmatic implementation order:

1. Introduce the shared runtime bundle/builder.
2. Move existing CLI or channel assembly logic onto it without changing behavior.
3. Move web chat onto it and verify parity for memory, tools, skills, and hooks.
4. Add web approval state and the `approval_request` + `/api/chat/approval` flow.
5. Extend frontend chat handling to render and resolve approvals.
6. Add regression tests around parity-sensitive behaviors.
