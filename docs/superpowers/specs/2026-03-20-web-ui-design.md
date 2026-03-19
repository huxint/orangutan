# Web Management UI — Design Spec

**Date:** 2026-03-20
**Status:** Draft

## Overview

Add a full-featured web management UI to orangutan: a chat interface for interacting with the AI plus admin pages for managing config, tools, agents, skills, and system status. Served by an embedded HTTP server (cpp-httplib) with a React SPA frontend.

## Goals

- Chat with orangutan from a browser with streaming responses, markdown rendering, tool call display, and session history
- Manage configuration, tools, agents, skills, and system health from admin pages
- Dark/light dual-theme with warm orange accent, concise visual style
- Single binary deployment — C++ serves both API and static frontend files

## Non-Goals

- Multi-user auth (single-user, local-network use only for now)
- Real-time collaboration
- Mobile-optimized layout (desktop-first)

## Architecture

### Backend (C++)

New module at `src/features/web/`:

- **web-server.hpp/.cpp** — cpp-httplib HTTP server, serves REST API + static files from `web/dist/`
- **web-routes.hpp/.cpp** — route handlers mapping REST endpoints to existing orangutan internals
- cpp-httplib added via FetchContent (header-only library)
- Server started via CLI flag: `orangutan --web` (default port 18080) or `orangutan --web --port PORT`
- Binds to `127.0.0.1` by default (localhost only) for security. Use `--web-host 0.0.0.0` to expose on LAN.
- Runs in its own thread, coexists with the existing REPL or runs standalone

### Concurrency Model

Each web chat session gets its own `AgentLoop` instance running on a dedicated worker thread. The web server maintains a `std::unordered_map<session_id, std::unique_ptr<AgentLoop>>` protected by a mutex. The REPL and web sessions are fully independent — they never share an `AgentLoop`. This avoids data races without requiring `AgentLoop` to be thread-safe internally.

Abort mechanism: each web `AgentLoop` is associated with a `std::atomic<bool> abort_requested` flag. `POST /api/chat/abort` sets this flag. The agent loop checks it between iterations and between tool calls, breaking out of the ReAct loop cleanly.

### SSE Streaming Bridge

`AgentLoop::run()` accepts a `StreamCallback`. For web chat:

1. `POST /api/chat` handler spawns the agent loop on a worker thread
2. The `StreamCallback` writes SSE-formatted events directly to the cpp-httplib chunked response writer
3. The HTTP response is `Content-Type: text/event-stream` from the start (SSE)
4. The connection stays open until the loop completes (`event: done`) or is aborted (`event: error`)

### Frontend (React SPA)

Directory: `web/` at project root.

- **Stack:** Vite + React 19 + TypeScript + Tailwind CSS v4
- **Components:** shadcn/ui for consistent, polished UI primitives
- **Routing:** React Router v7 with hash-based routing (`/#/chat`, `/#/config`, etc.) — avoids server-side route handling
- **Theming:** CSS custom properties on `<html data-theme="dark|light">`, toggled via localStorage
- **Dev:** `pnpm dev` with hot-reload, Vite proxy forwards `/api/*` to C++ backend (default `localhost:18080`)
- **Build:** `pnpm build` → `web/dist/` (static files served by C++)

### Build Integration

Frontend and backend builds are independent:

1. **Dev workflow:** Run `pnpm dev` in `web/` (Vite dev server with HMR) + `orangutan --web` separately. Vite proxies API calls to the C++ backend.
2. **Production build:** Run `pnpm build` in `web/` first, then `cmake --build`. The C++ binary serves `web/dist/` at runtime (path resolved relative to executable or via `--web-dir`).
3. CMake does NOT call npm — the two build systems are decoupled. A convenience script `scripts/build-all.sh` runs both in sequence.

### Layout

Persistent sidebar layout:

```
┌─────────────────────────────────────────────┐
│ 🦧 Orangutan              model-name  [☀/🌙]│
├──────────┬──────────────────────────────────┤
│ + New    │                                  │
│ Today    │   Main Content Area              │
│ Yesterday│   (Chat / Admin views)           │
│          │                                  │
│ ──ADMIN──│                                  │
│ Config   │                                  │
│ Tools    │                                  │
│ Agents   │                                  │
│ Skills   │                                  │
│ System   │                                  │
├──────────┤──────────────────────────────────│
│          │ [Message input...]        [Send] │
└──────────┴──────────────────────────────────┘
```

- Sidebar: session list (grouped by date) + admin navigation
- Top bar: logo, current model badge, theme toggle
- Main area: switches between chat view and admin pages via React Router
- Chat input: sticky at bottom with send button

## Visual Style

### Color Tokens

| Token | Dark | Light |
|-------|------|-------|
| `--bg` | #242424 | #faf8f6 |
| `--bg-surface` | #1e1e1e | #f0ebe5 |
| `--bg-elevated` | #2a2a2a | #ffffff |
| `--border` | #333333 | #e8e0d8 |
| `--text` | #d4d4d4 | #3a352e |
| `--text-muted` | #777777 | #9a8e82 |
| `--accent` | #f97316 | #e8720c |
| `--accent-bg` | #f9731611 | #e8720c11 |

### Design Principles

- **Concise** — minimal chrome, thin 1px dividers, no heavy borders or shadows
- **Warm neutrals** — cream-tinted grays in light mode, neutral grays in dark mode
- **Orange accent** — used sparingly for active states, CTAs, AI identity
- **Theme toggle** — sun/moon icon in top bar, state persisted to localStorage

## API Surface

### Chat

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/chat` | Send message; returns SSE stream of content blocks |
| `POST` | `/api/chat/abort` | Abort current generation |

**`POST /api/chat` request body:**
```json
{
  "session_id": "optional-uuid",  // omit or null to create new session
  "message": "user message text"
}
```

If `session_id` is omitted, a new session is created and its ID is sent as the first SSE event (`event: session`). If provided, the existing session's history is loaded and the message is appended.

**Response:** `Content-Type: text/event-stream` (SSE from the start)

SSE event types:
- `event: session` — `{"session_id": "..."}` (first event, always sent)
- `event: text` — `{"text": "..."}`
- `event: tool_use` — `{"id": "...", "name": "...", "input": {...}}`
- `event: tool_result` — `{"tool_use_id": "...", "content": "...", "is_error": false}`
- `event: done` — `{"stop_reason": "end_turn"}`
- `event: error` — `{"error": "message"}`

**`POST /api/chat/abort`:** Sets `abort_requested` flag on the active `AgentLoop` for the given session. Body: `{"session_id": "..."}`. Returns 200 on success, 404 if no active loop for that session.

### Sessions

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/sessions` | List sessions (id, created_at, model, message_count) |
| `GET` | `/api/sessions/:id` | Load session messages |
| `DELETE` | `/api/sessions/:id` | Delete a session |

Note: `SessionInfo` in session-store has `id`, `created_at`, `model`, `scope_key`, `message_count`. The API returns these fields directly (no `title` field — sessions are identified by date + first message preview, computed client-side from the first user message in the session).

### Config

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/config` | Read current config as JSON |
| `PUT` | `/api/config` | Update config (validates, writes config.toml) |

**New backend code needed:** `Config::save_to(path)` method that serializes the Config struct back to TOML format via toml++. This is new code — the current Config only supports `load`/`load_from` (read-only).

### Tools, Agents, Skills, System

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/tools` | List registered tools (name, description, source) |
| `GET` | `/api/agents` | List configured agents with their settings |
| `GET` | `/api/skills` | List loaded skills |
| `GET` | `/api/system` | System status (uptime, active sessions, provider health) |

**Backend data sources:**
- **Tools:** `ToolRegistry` already maintains a map of `ToolDef` — serialize its contents
- **Agents:** `Config::agents` map — serialize directly from config
- **Skills:** `SkillLoader` maintains loaded skills — add a `list()` method returning skill names and paths
- **System:** New `WebServer::system_status()` aggregating: process uptime (`std::chrono::steady_clock` from startup), active web session count (from session map size), and last provider response metadata (latency/status stored by the provider after each call)

## Frontend Pages

### Chat View (default, route: `/#/chat` or `/#/chat/:sessionId`)

- Message list with role-based styling (user vs assistant)
- Assistant messages render markdown with syntax-highlighted code blocks
- Tool use blocks: collapsible cards showing tool name, input, and result
- Streaming: text appears incrementally via SSE EventSource API
- Input area: auto-resizing textarea, Enter to send, Shift+Enter for newline

### Config Page (`/#/config`)

- Form rendering of config.toml sections (agent, tools, permissions, session, memory, security)
- Grouped by section with collapsible panels
- Save button validates and writes via `PUT /api/config`
- Secret fields (api_key) shown as masked with reveal toggle

### Tools Page (`/#/tools`)

- Table listing all registered tools: name, description, source (builtin/script/mcp)
- MCP servers shown with connection status
- Custom script tools with their command and timeout

### Agents Page (`/#/agents`)

- Card layout showing each configured agent with provider, model, base_url
- Default agent highlighted
- Subagent relationships displayed

### Skills Page (`/#/skills`)

- List of loaded skills with name and source path
- Status indicator (loaded/error)

### System Page (`/#/system`)

- Process uptime, active web session count
- Provider health (last API response time/status)
- Cron jobs list with next-run time
- Heartbeat job status

## Data Flow

1. **Chat:** Browser → `POST /api/chat` with `{session_id?, message}` → C++ spawns worker thread with new/existing AgentLoop → `StreamCallback` writes SSE events to chunked response → React `EventSource` renders incrementally
2. **Config:** `GET /api/config` → C++ serializes Config to JSON → React form → `PUT /api/config` → C++ validates + calls `Config::save_to()` to write config.toml
3. **Sessions:** `GET /api/sessions` → C++ queries SQLite → React sidebar list → click loads via `GET /api/sessions/:id`
4. **Theme:** localStorage only, CSS variables swap on `<html data-theme="dark|light">`

## Error Handling

- API errors: `{"error": "message"}` with appropriate HTTP status codes
- SSE mid-stream errors: `event: error` sent before stream closes
- Network disconnect: React reconnects SSE with exponential backoff (1s, 2s, 4s, max 30s)
- Config validation: inline error messages in the form, 400 response with field-level errors

## Security

- Web server binds to `127.0.0.1` by default — not accessible from the network
- `--web-host 0.0.0.0` explicitly required to expose on LAN (logged as a warning)
- CORS: In dev mode (Vite proxy), no CORS needed — same origin. In production, static files served from same origin — no CORS needed. If `--web-host` is used, `Access-Control-Allow-Origin` header set to the request origin (localhost variants only).
- No auth in v1 — acceptable since localhost-only. Future: optional bearer token via config.

## Testing

- **Backend:** GoogleTest for web route handlers (mock HTTP request/response)
- **Frontend:** Vitest + React Testing Library for component unit tests
- **Integration:** manual testing initially; Playwright E2E deferred to later
- Existing test suite (52+ tests) unaffected — web module is purely additive

## Dependencies

### Backend (new)
- **cpp-httplib** — header-only HTTP server (FetchContent)

### Frontend (new, in `web/`)
- **React 19** + **TypeScript**
- **Vite** — build tool
- **Tailwind CSS v4** — utility-first styling (CSS-based `@theme` config, no JS config file)
- **shadcn/ui** — component library
- **React Router v7** — hash-based client-side routing
- **pnpm** — package manager

## File Structure

```
src/features/web/
  web-server.hpp        # HttpServer class, start/stop, static file serving
  web-server.cpp
  web-routes.hpp        # Route handler declarations
  web-routes.cpp        # Route implementations (chat, config, sessions, etc.)

web/
  package.json
  vite.config.ts
  tsconfig.json
  index.html
  src/
    main.tsx
    App.tsx
    theme.ts            # Theme toggle logic + CSS variable definitions
    app.css             # Tailwind v4 @theme config + color tokens
    api/
      client.ts         # Fetch wrapper, SSE helper
    components/
      layout/
        Sidebar.tsx
        TopBar.tsx
      chat/
        ChatView.tsx
        MessageList.tsx
        MessageBubble.tsx
        ToolCallCard.tsx
        ChatInput.tsx
      admin/
        ConfigPage.tsx
        ToolsPage.tsx
        AgentsPage.tsx
        SkillsPage.tsx
        SystemPage.tsx

scripts/
  build-all.sh          # Convenience: pnpm build in web/ then cmake --build
```

## CLI Integration

New flags in `src/main.cpp`:

- `--web` — start the web server (alongside REPL or standalone)
- `--web-only` — start only the web server, no REPL (headless mode)
- `--port PORT` — web server port (default: 18080)
- `--web-host HOST` — bind address (default: 127.0.0.1)
- `--web-dir PATH` — path to frontend static files (default: `web/dist/` relative to executable)

The web server runs in a separate thread. With `--web`, both the REPL and web server run concurrently. With `--web-only`, the main thread blocks on the web server (no REPL). The `-m` flag retains its existing "single message" semantics and is unrelated to web mode.
