# Agent-Centric Web Chat — Design Spec

**Date:** 2026-03-20
**Status:** Approved

## Overview

Refactor the web chat UI from a session-centric layout into an agent-centric workspace:

- The left sidebar shows configured agents and their subagent hierarchy, not saved sessions.
- Entering an agent opens that agent's chat workspace.
- The top-right of the agent chat view shows all sessions associated with that agent, including sessions created from web, CLI, and channel runtimes.
- Channel-backed sessions are visible in web but read-only.

This change affects both backend session metadata and frontend routing/layout. It is a focused redesign of the web chat workflow, not a general admin UI overhaul.

## Goals

- Replace the sidebar session list with an agent tree built from configured agent hierarchy.
- Scope the chat experience to the currently selected agent.
- Show all sessions for the selected agent in a top-right session switcher.
- Include sessions created outside the web UI, especially channel conversations.
- Clearly mark channel sessions as readable but not recommended for continued chatting in web.
- Keep subagent hierarchy based on configuration, not runtime discovery.

## Non-Goals

- No compatibility or migration logic for old session rows without new metadata.
- No editing or continuing channel sessions from the web UI.
- No runtime-generated hierarchy based on spawned child runs.
- No redesign of non-chat admin pages beyond what is needed for navigation consistency.

## Key Decisions

### Agent Tree Comes From Effective Config, Not Raw Maps

The web UI must not rely on `Config::agents` alone. The `default` agent may be provided implicitly by the top-level `[agent]` section or explicitly as `[agents.default]`. The web layer therefore needs an effective agent catalog that:

- always exposes a `default` agent node,
- merges effective runtime settings used for execution,
- includes each agent's declared `subagents`,
- identifies root nodes as agents that are not referenced as a child of another agent.

This keeps the web tree aligned with actual runnable agents.

### Session Ownership Is Explicit

Session ownership must be stored explicitly with an `agent_key` field. Using only `scope_key` is too implicit and fails the new UX for:

- default agent sessions,
- agent-scoped listing across multiple runtimes,
- validating that a session belongs to the selected agent.

### Session Origin Is Explicit

To support read-only channel sessions and source labels in the session switcher, each session row also stores:

- `origin_kind`: `web` | `cli` | `channel`
- `origin_ref`: optional runtime/source identifier

Examples:

- `web` session: `origin_kind = "web"`, `origin_ref = ""`
- `cli` session: `origin_kind = "cli"`, `origin_ref = "cli:local"`
- `channel` session: `origin_kind = "channel"`, `origin_ref = "<jid or runtime key>"`

The web UI derives `read_only = true` whenever `origin_kind == "channel"`.

## Architecture

### Backend Data Model

Extend the `sessions` table with the following non-null columns:

- `agent_key TEXT NOT NULL`
- `origin_kind TEXT NOT NULL`
- `origin_ref TEXT NOT NULL DEFAULT ''`

`SessionInfo` should be extended to expose those fields to the web layer:

- `agent_key`
- `origin_kind`
- `origin_ref`

No backward-compatibility behavior is required. Existing development data may be discarded.

### SessionStore API

Refactor `SessionStore` APIs to accept explicit session metadata, rather than only `model` and `scope_key`.

Introduce a small metadata object, for example:

```cpp
struct SessionMetadata {
    std::string model;
    std::string scope_key;
    std::string agent_key;
    std::string origin_kind;
    std::string origin_ref;
};
```

Use it for:

- `save(...)`
- `create_empty(...)`
- `update(...)`
- `append(...)`

Add query helpers for the new web UX:

- `list_sessions_for_agent(const std::string &agent_key)`
- `load_session_info(const std::string &session_id)` or equivalent metadata lookup
- `session_belongs_to_agent(const std::string &session_id, const std::string &agent_key)`

The session list query should remain sorted by newest first.

### Runtime Integration

All runtime entry points that persist sessions must write agent and origin metadata:

- Web chat writes `agent_key=<selected agent>`, `origin_kind=web`
- CLI writes `agent_key=<selected agent>`, `origin_kind=cli`
- Channel runtime writes `agent_key=<resolved agent>`, `origin_kind=channel`

This is the only way the web session switcher can truthfully show "all sessions for this agent".

### Effective Agent Catalog

Add a backend helper that returns the effective web-facing agent catalog. Each node should contain:

- `key`
- `provider`
- `model`
- `base_url`
- `workspace`
- `edit_mode`
- `subagents`

For the web UI, `default` must always appear as an agent node even when it is sourced from the top-level config rather than an explicit `[agents.default]` section.

The API may return a flat list with `subagents`, and the frontend can build the tree, or it may return nested nodes directly. The flat shape is preferred because it is simpler to test and keeps tree rendering in the frontend.

## Web API

### `GET /api/agents`

Returns the effective runnable agent catalog, not just raw `Config::agents`.

Example response:

```json
[
  {
    "key": "default",
    "provider": "anthropic",
    "model": "claude-sonnet-4-20250514",
    "base_url": "https://api.anthropic.com",
    "workspace": "/workspace/default",
    "edit_mode": "hashline",
    "subagents": ["coder"]
  },
  {
    "key": "coder",
    "provider": "openai",
    "model": "gpt-5.4",
    "base_url": "https://api.openai.com/v1",
    "workspace": "/workspace/coder",
    "edit_mode": "hashline",
    "subagents": []
  }
]
```

### `GET /api/agents/:agentKey/sessions`

Returns only sessions that belong to the selected agent.

Each item should include:

- `id`
- `created_at`
- `model`
- `message_count`
- `agent_key`
- `origin_kind`
- `origin_ref`
- `origin_label`
- `read_only`
- `preview`

`preview` is derived server-side from the first meaningful user text in the session. No dedicated preview column is needed in v1.

`origin_label` is presentation-oriented text such as:

- `Web`
- `CLI`
- `Channel · qqbot:c2c:alice`

### `GET /api/agents/:agentKey/sessions/:sessionId`

Loads the session content plus metadata, but only if the session belongs to the selected agent.

If the session belongs to another agent, return `404` rather than leaking cross-agent existence.

Response includes:

- `id`
- `agent_key`
- `origin_kind`
- `origin_ref`
- `read_only`
- `messages`

### `POST /api/chat`

Extend the request body to include:

```json
{
  "agent_key": "coder",
  "session_id": "optional-session-id",
  "message": "hello"
}
```

Behavior:

- Resolve the selected agent before constructing provider/tools/agent loop.
- If `session_id` is omitted, create a new web session with `origin_kind=web`.
- If `session_id` is provided, verify it belongs to `agent_key`.
- If the session is read-only, reject sending with a client-visible error.

### Optional Deletion

If session deletion remains supported in the new UI, prefer an agent-scoped route:

- `DELETE /api/agents/:agentKey/sessions/:sessionId`

The same ownership check applies.

## Frontend Information Architecture

### Sidebar

The persistent sidebar changes from:

- navigation + session list

to:

- navigation + agent tree

Rules:

- Root nodes are agents not referenced by any other agent's `subagents`.
- Child nodes are rendered indented under their parent.
- The selected agent is highlighted.
- Sidebar no longer owns session navigation.

### Routes

Refactor chat routes to be agent-first:

- `/#/chat/:agentKey`
- `/#/chat/:agentKey/:sessionId`

Behavior:

- `/#/chat/:agentKey` opens a new chat workspace for that agent.
- `/#/chat/:agentKey/:sessionId` opens a specific session under that agent.
- Unknown `agentKey` shows an error/empty state and does not silently redirect.

### Chat Header

The chat page gets a real header area above the message list containing:

- current agent key,
- model/provider summary,
- read-only banner when viewing a channel session,
- a top-right session switcher.

### Session Switcher

The session switcher is a top-right panel or popover in the current agent page.

It shows all sessions for the selected agent, grouped by date:

- Today
- Yesterday
- Older

Each row shows:

- preview text,
- created time,
- source label,
- read-only indicator when applicable.

Actions:

- open a session,
- start a new chat for the current agent.

The switcher must never show sessions from another agent.

### Chat Input Rules

- Normal web and CLI sessions can continue chatting from the web UI.
- Channel sessions are visible but read-only.
- For read-only sessions, the input area is hidden or disabled with a clear explanation:
  `Channel session · read only`
  `View history here, but continue the conversation from its original channel.`

## Message Loading and Rendering

The recent frontend work that merges assistant blocks and embeds tool cards inside assistant messages stays valid. This redesign changes session selection and agent context, not the assistant message rendering model.

The only chat-view state changes needed are:

- selected `agentKey`,
- current `sessionId`,
- session metadata including `read_only`,
- per-agent session switcher contents.

## Error Handling

- Unknown agent: `404` or client-visible empty state.
- Session does not belong to selected agent: `404`.
- Attempt to send on a read-only session: `409` with a clear error message.
- No sessions for an agent: session switcher shows an empty state, but new chat remains available.

## Testing

### Backend

Add or update GoogleTest coverage for:

- session schema includes `agent_key`, `origin_kind`, `origin_ref`
- listing sessions by agent returns only matching rows
- web chat creation stores the correct `agent_key` and `origin_kind=web`
- channel persistence stores `origin_kind=channel`
- `GET /api/agents` returns effective agent catalog including hierarchy metadata
- cross-agent session access is rejected
- read-only metadata is returned for channel sessions

### Frontend

At minimum:

- `pnpm build`
- manual verification that sidebar shows agents instead of sessions
- manual verification that switching agents changes the session switcher contents
- manual verification that channel sessions render as read-only
- manual verification that `/chat/:agentKey` starts a new session for that agent

No new frontend test stack is required as part of this change.

## Implementation Areas

Expected code areas:

- `src/infra/storage/session-store.hpp`
- `src/infra/storage/session-store.cpp`
- `src/features/web/web-routes.hpp`
- `src/features/web/web-routes.cpp`
- `src/features/web/web-server.cpp`
- `src/app/bootstrap.cpp`
- `src/app/session-workflow.cpp`
- `src/app/single-shot.cpp`
- `src/app/channel-serve.cpp`
- `web/src/App.tsx`
- `web/src/components/layout/Sidebar.tsx`
- `web/src/components/chat/ChatView.tsx`
- new chat header / session switcher components as needed

## Planning Notes

This spec is ready for implementation planning. The work is cohesive enough for a single plan because it is one feature slice:

- store session ownership metadata,
- expose agent/session APIs,
- reshape web navigation around agents,
- enforce read-only channel viewing.

There are no intentional placeholders, migration tasks, or deferred compatibility work in this spec.
