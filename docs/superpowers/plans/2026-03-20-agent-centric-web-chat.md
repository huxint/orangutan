# Agent-Centric Web Chat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use @superpowers:subagent-driven-development (recommended) or @superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reshape the web chat into an agent-centric workspace with a sidebar agent tree, per-agent session switcher, explicit session ownership/origin metadata, and read-only channel session viewing.

**Architecture:** Persist `agent_key` plus session origin metadata in `SessionStore`, propagate it from web/CLI/channel runtimes, and expose agent-scoped web APIs that validate session ownership. Refactor the frontend around `/chat/:agentKey(/:sessionId)` so the sidebar selects agents while a chat header owns the current agent's sessions and read-only state.

**Tech Stack:** C++23, SQLite, cpp-httplib, GoogleTest, React 19, TypeScript, React Router v7, Tailwind CSS v4, pnpm/Vite

---

## File Structure

### Backend

- `src/infra/storage/session-store.hpp`
  Adds explicit session metadata types and agent-scoped query helpers.
- `src/infra/storage/session-store.cpp`
  Owns schema changes, metadata persistence, preview extraction, and agent/origin-aware queries.
- `src/app/session-workflow.cpp`
  Central place for saving/updating CLI sessions; must stop treating `scope_key` as the only ownership signal.
- `src/app/session-workflow.hpp`
  Keeps helper signatures aligned with the new metadata object.
- `src/app/single-shot.cpp`
  Persists CLI single-message sessions with explicit `agent_key` and `origin_kind=cli`.
- `src/app/bootstrap.cpp`
  Provides effective runnable agent catalog details, including `default`, for both runtime setup and web-facing APIs.
- `src/app/channel-serve.cpp`
  Persists channel sessions with `origin_kind=channel` and the resolved agent key.
- `src/features/web/web-routes.hpp`
  Declares new agent-scoped session routes and chat contracts.
- `src/features/web/web-routes.cpp`
  Implements `/api/agents`, `/api/agents/:agentKey/sessions`, `/api/agents/:agentKey/sessions/:sessionId`, and agent-aware `/api/chat`.
- `src/features/web/web-server.cpp`
  Wires the new routes into the embedded HTTP server.

### Frontend

- `web/src/App.tsx`
  Changes chat routing from session-first to agent-first.
- `web/src/api/client.ts`
  Adds agent-scoped fetch helpers and sends `agent_key` with chat requests.
- `web/src/components/layout/Sidebar.tsx`
  Stops rendering sessions and becomes an agent-tree host.
- `web/src/components/layout/AgentTree.tsx`
  New focused component for rendering hierarchical agents and current selection.
- `web/src/components/chat/ChatView.tsx`
  Loads agent metadata, agent-scoped sessions, session read-only state, and current session messages.
- `web/src/components/chat/ChatHeader.tsx`
  New focused header component showing current agent, model/provider, read-only banner, and session switcher trigger.
- `web/src/components/chat/AgentSessionSwitcher.tsx`
  New top-right session switcher for the currently selected agent.
- `web/src/components/chat/ChatInput.tsx`
  Accepts read-only/disabled messaging state from `ChatView`.
- `web/src/components/chat/types.ts`
  Extends frontend chat/session types with agent/session metadata.

### Tests

- `tests/infra/session-store-test.cpp`
  Verifies schema and metadata persistence.
- `tests/app/session-workflow-test.cpp`
  Verifies CLI session persistence uses explicit metadata.
- `tests/app/channel-serve-test.cpp`
  Verifies channel session persistence writes agent/origin metadata.
- `tests/features/web-routes-test.cpp`
  Verifies agent-scoped list/load/delete endpoints.
- `tests/features/web-chat-test.cpp`
  Verifies `/api/chat` requires `agent_key`, honors agent ownership, and rejects read-only sessions.

## Task 1: Extend Session Metadata In Storage

**Files:**
- Modify: `src/infra/storage/session-store.hpp`
- Modify: `src/infra/storage/session-store.cpp`
- Test: `tests/infra/session-store-test.cpp`

- [ ] **Step 1: Write failing storage tests for explicit session metadata**

Add tests in `tests/infra/session-store-test.cpp` that save sessions with explicit metadata and assert:

```cpp
const SessionMetadata meta{
    .model = "test-model",
    .scope_key = "agent:coder",
    .agent_key = "coder",
    .origin_kind = "channel",
    .origin_ref = "qqbot:c2c:alice",
};
```

Cover:
- `save()` and `create_empty()` persist `agent_key`, `origin_kind`, `origin_ref`
- `list_sessions_for_agent("coder")` returns only coder sessions
- `session_belongs_to_agent(id, "coder")` returns true and false for mismatches

- [ ] **Step 2: Run the storage tests to verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "SessionStoreTest"
```

Expected: FAIL with missing `SessionMetadata`/query helpers or schema assertions failing.

- [ ] **Step 3: Implement explicit session metadata and agent-scoped queries**

Modify `src/infra/storage/session-store.hpp` and `src/infra/storage/session-store.cpp` to:

- add `SessionMetadata`
- extend `SessionInfo` with `agent_key`, `origin_kind`, `origin_ref`
- migrate `save/create_empty/update/append` to accept metadata
- add `list_sessions_for_agent(...)`
- add `session_belongs_to_agent(...)`
- keep existing `scope_key` behavior intact where still needed for CLI/channel restore logic

Use a schema shape equivalent to:

```sql
ALTER TABLE sessions ADD COLUMN agent_key TEXT NOT NULL DEFAULT '';
ALTER TABLE sessions ADD COLUMN origin_kind TEXT NOT NULL DEFAULT 'cli';
ALTER TABLE sessions ADD COLUMN origin_ref TEXT NOT NULL DEFAULT '';
```

Because this is development-stage only, it is acceptable to make the schema authoritative for new data and skip old-data compatibility logic.

- [ ] **Step 4: Re-run the storage tests to verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "SessionStoreTest"
```

Expected: PASS for all session store tests, including the new metadata coverage.

- [ ] **Step 5: Commit**

```bash
git add src/infra/storage/session-store.hpp src/infra/storage/session-store.cpp tests/infra/session-store-test.cpp
git commit -m "feat: persist agent-aware session metadata"
```

## Task 2: Propagate Agent And Origin Metadata Through Runtime Persistence

**Files:**
- Modify: `src/app/session-workflow.hpp`
- Modify: `src/app/session-workflow.cpp`
- Modify: `src/app/single-shot.cpp`
- Modify: `src/app/channel-serve.cpp`
- Modify: `src/app/bootstrap.cpp`
- Test: `tests/app/session-workflow-test.cpp`
- Test: `tests/app/channel-serve-test.cpp`
- Test: `tests/app/bootstrap-test.cpp`

- [ ] **Step 1: Write failing runtime tests for metadata propagation**

Add targeted tests that assert:

- CLI/session workflow persistence passes `agent_key=<selected agent>` and `origin_kind=cli`
- channel persistence writes `origin_kind=channel`
- the effective agent catalog/runtime config always includes a usable `default` agent with declared `subagents`

Prefer assertions like:

```cpp
EXPECT_EQ(saved_session.agent_key, "default");
EXPECT_EQ(saved_session.origin_kind, "cli");
EXPECT_EQ(saved_session.origin_ref, "cli:local");
```

- [ ] **Step 2: Run the runtime tests to verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "SessionWorkflowTest|ChannelServeTest|BootstrapTest"
```

Expected: FAIL because runtime persistence still writes only `model` and `scope_key`.

- [ ] **Step 3: Thread session metadata through CLI and channel save paths**

Update the runtime helpers so saving a session always uses explicit metadata:

- `persist_session(...)`
- `start_new_session(...)`
- `run_single_message(...)`
- channel runtime save/update/bind paths

Adopt a consistent mapping:

```cpp
SessionMetadata{
    .model = active_model,
    .scope_key = runtime_scope,
    .agent_key = runtime_agent_key,
    .origin_kind = "cli" | "channel" | "web",
    .origin_ref = runtime_key_or_jid,
}
```

Also add or extract a helper in `src/app/bootstrap.cpp` that builds the effective agent catalog used by the web layer so `default` is always present even when sourced from top-level `[agent]`.

- [ ] **Step 4: Re-run the runtime tests to verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "SessionWorkflowTest|ChannelServeTest|BootstrapTest"
```

Expected: PASS with explicit metadata assertions succeeding.

- [ ] **Step 5: Commit**

```bash
git add src/app/session-workflow.hpp src/app/session-workflow.cpp src/app/single-shot.cpp src/app/channel-serve.cpp src/app/bootstrap.cpp tests/app/session-workflow-test.cpp tests/app/channel-serve-test.cpp tests/app/bootstrap-test.cpp
git commit -m "feat: propagate session agent metadata across runtimes"
```

## Task 3: Add Agent-Scoped Web APIs And Agent-Aware Chat

**Files:**
- Modify: `src/features/web/web-routes.hpp`
- Modify: `src/features/web/web-routes.cpp`
- Modify: `src/features/web/web-server.cpp`
- Test: `tests/features/web-routes-test.cpp`
- Test: `tests/features/web-chat-test.cpp`

- [ ] **Step 1: Write failing web route tests for agent-scoped APIs**

Extend `tests/features/web-routes-test.cpp` and `tests/features/web-chat-test.cpp` to cover:

- `GET /api/agents` returns effective agents plus `subagents`
- `GET /api/agents/:agentKey/sessions` returns only sessions for that agent
- `GET /api/agents/:agentKey/sessions/:sessionId` returns `404` for cross-agent access
- `POST /api/chat` requires `agent_key`
- `POST /api/chat` rejects sending into a read-only channel session

Use response expectations like:

```cpp
EXPECT_EQ(body[0]["agent_key"], "coder");
EXPECT_EQ(body[0]["read_only"], true);
EXPECT_EQ(res->status, 409);
```

- [ ] **Step 2: Run the web tests to verify they fail**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: FAIL because the agent-scoped routes do not exist and `/api/chat` ignores `agent_key`.

- [ ] **Step 3: Implement agent-scoped routes and ownership checks**

Modify `src/features/web/web-routes.hpp`, `src/features/web/web-routes.cpp`, and `src/features/web/web-server.cpp` to:

- expose `GET /api/agents`
- expose `GET /api/agents/:agentKey/sessions`
- expose `GET /api/agents/:agentKey/sessions/:sessionId`
- optionally expose `DELETE /api/agents/:agentKey/sessions/:sessionId`
- require `agent_key` on `POST /api/chat`
- resolve provider/model/system prompt/workspace from the selected agent
- reject reads and writes where a session does not belong to that agent
- mark `origin_kind=channel` sessions as read-only in the response payload

Keep existing generic session endpoints only if they remain useful elsewhere; the new chat UI must consume the agent-scoped APIs.

- [ ] **Step 4: Re-run the web tests to verify they pass**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "WebRoutesTest|WebChatTest"
```

Expected: PASS for the new route coverage and chat ownership/read-only checks.

- [ ] **Step 5: Commit**

```bash
git add src/features/web/web-routes.hpp src/features/web/web-routes.cpp src/features/web/web-server.cpp tests/features/web-routes-test.cpp tests/features/web-chat-test.cpp
git commit -m "feat: add agent-scoped web chat APIs"
```

## Task 4: Replace Sidebar Sessions With An Agent Tree

**Files:**
- Modify: `web/src/App.tsx`
- Modify: `web/src/components/layout/Sidebar.tsx`
- Create: `web/src/components/layout/AgentTree.tsx`
- Modify: `web/src/api/client.ts`

- [ ] **Step 1: Write the failing frontend scaffold for agent-first routing**

Start by updating `web/src/App.tsx` and `web/src/components/layout/Sidebar.tsx` to reference the new route shape and `AgentTree` component before the implementation exists:

```tsx
<Route path="/chat/:agentKey" element={<ChatView />} />
<Route path="/chat/:agentKey/:sessionId" element={<ChatView />} />
```

and:

```tsx
import { AgentTree } from './AgentTree'
```

- [ ] **Step 2: Run the frontend build to verify it fails**

Run:

```bash
cd web
pnpm build
```

Expected: FAIL with missing `AgentTree` import and/or route/prop mismatches.

- [ ] **Step 3: Implement the sidebar agent tree and agent fetch helpers**

Create `web/src/components/layout/AgentTree.tsx` and update `web/src/components/layout/Sidebar.tsx` to:

- fetch `/api/agents`
- compute root nodes from `subagents`
- render hierarchical indentation
- highlight the current agent
- remove the old sessions section entirely
- make “New Chat” navigate to the current agent if one is selected, otherwise to `default`

Add any small client helpers to `web/src/api/client.ts` needed for:

```ts
getAgents(): Promise<AgentSummary[]>
```

- [ ] **Step 4: Re-run the frontend build to verify it passes**

Run:

```bash
cd web
pnpm build
```

Expected: PASS with the sidebar compiling against the new agent-first routes.

- [ ] **Step 5: Commit**

```bash
git add web/src/App.tsx web/src/components/layout/Sidebar.tsx web/src/components/layout/AgentTree.tsx web/src/api/client.ts
git commit -m "feat: switch web sidebar to agent tree navigation"
```

## Task 5: Add Agent Header, Session Switcher, And Read-Only Chat Behavior

**Files:**
- Modify: `web/src/components/chat/ChatView.tsx`
- Modify: `web/src/components/chat/ChatInput.tsx`
- Modify: `web/src/components/chat/types.ts`
- Modify: `web/src/api/client.ts`
- Create: `web/src/components/chat/ChatHeader.tsx`
- Create: `web/src/components/chat/AgentSessionSwitcher.tsx`

- [ ] **Step 1: Write the failing frontend scaffold for the chat header and session switcher**

Update `web/src/components/chat/ChatView.tsx` to reference the new components and metadata before they exist:

```tsx
<ChatHeader
  agent={agent}
  session={sessionMeta}
  sessions={agentSessions}
  onSelectSession={...}
  onStartNewChat={...}
/>
```

Also extend the chat request payload shape in `web/src/api/client.ts` to require `agent_key`.

- [ ] **Step 2: Run the frontend build to verify it fails**

Run:

```bash
cd web
pnpm build
```

Expected: FAIL with missing `ChatHeader`/`AgentSessionSwitcher` modules and unresolved metadata types.

- [ ] **Step 3: Implement agent-scoped chat state, header UI, and read-only rules**

Create `web/src/components/chat/ChatHeader.tsx` and `web/src/components/chat/AgentSessionSwitcher.tsx`, then update `web/src/components/chat/ChatView.tsx`, `web/src/components/chat/ChatInput.tsx`, `web/src/components/chat/types.ts`, and `web/src/api/client.ts` to:

- load the selected agent from the route
- fetch the selected agent's session list from `/api/agents/:agentKey/sessions`
- load session content from `/api/agents/:agentKey/sessions/:sessionId`
- send `agent_key` with `/api/chat`
- show model/provider summary in the header
- show a top-right session switcher grouped by date
- mark channel sessions as read-only
- hide or disable message sending for read-only sessions with explicit copy:

```text
Channel session · read only
View history here, but continue the conversation from its original channel.
```

- [ ] **Step 4: Re-run the frontend build and perform focused manual verification**

Run:

```bash
cd web
pnpm build
```

Expected: PASS

Then manually verify:

- `/chat/default` starts a new default-agent session
- switching to another agent changes only that agent's session switcher contents
- opening a channel session shows history but disables sending
- opening a web/CLI session still allows sending

- [ ] **Step 5: Commit**

```bash
git add web/src/components/chat/ChatView.tsx web/src/components/chat/ChatInput.tsx web/src/components/chat/types.ts web/src/api/client.ts web/src/components/chat/ChatHeader.tsx web/src/components/chat/AgentSessionSwitcher.tsx
git commit -m "feat: add per-agent session switcher to web chat"
```

## Task 6: Final Integration Verification

**Files:**
- Modify: `tests/features/web-routes-test.cpp`
- Modify: `tests/features/web-chat-test.cpp`
- Modify: `tests/infra/session-store-test.cpp`
- Modify: `tests/app/session-workflow-test.cpp`
- Modify: `tests/app/channel-serve-test.cpp`
- Verify: `web/`

- [ ] **Step 1: Run the full targeted backend suite**

Run:

```bash
cmake --build build --target orangutan_tests
ctest --test-dir build --output-on-failure -R "SessionStoreTest|SessionWorkflowTest|ChannelServeTest|BootstrapTest|WebRoutesTest|WebChatTest"
```

Expected: PASS with no agent/session ownership regressions.

- [ ] **Step 2: Run the frontend production build**

Run:

```bash
cd web
pnpm build
```

Expected: PASS

- [ ] **Step 3: Perform end-to-end smoke checks**

Verify manually:

- sidebar shows configured agent hierarchy instead of sessions
- agent switch does not leak other agents' sessions
- right-top session switcher shows web/CLI/channel entries for the selected agent
- channel sessions are readable and clearly read-only
- non-channel sessions remain writable

- [ ] **Step 4: Commit the final integration pass**

If Step 3 exposed any final mismatches, stage the touched files and commit the fixes:

```bash
git add <touched-files>
git commit -m "fix: polish agent-centric web chat integration"
```
