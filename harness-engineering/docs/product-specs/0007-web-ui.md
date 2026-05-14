# 0007 — Web UI

## User Problem

Operators want a browser-based UI for: live chat with the agent, session browsing,
admin operations, and inspecting orchestration DAGs and automation jobs. The legacy
`orangutan/` had a cpp-httplib-based web UI; v2 keeps that as a starting point but
prepares for a higher-concurrency replacement.

## Scope (v1)

- `oran-web::Server` running on cpp-httplib (carried over).
- Routes:
  - `GET  /` — single-page app shell.
  - `POST /chat` — send a message.
  - `GET  /chat/stream` — SSE stream of agent output.
  - `GET  /sessions` — list sessions.
  - `GET  /sessions/:id` — load a session.
  - `GET  /admin/audit` — recent audit log entries.
  - `GET  /admin/automation` — list automation jobs.
  - `POST /admin/automation` — schedule/modify a job.
- Token-based auth (header `X-Orangutan-Token`, value from config).
- CORS configurable; default allow `localhost:*`.
- SSE streaming wires to the agent loop's `EventSink`.
- Web hook for `provider.cost_threshold` to display spend warnings in the UI.

## Scope (v1.1)

- WebSocket fallback for browsers without reliable SSE.
- Admin endpoints for hooks (list + test-fire).
- Conversation-DAG renderer for orchestration teams.

## Scope (v2)

- Replace cpp-httplib server with an asio-based implementation supporting hundreds
  of concurrent SSE clients.
- Authentication via OAuth or session cookies.
- Per-user permissions and audit scoping.

## Out Of Scope

- Mobile-responsive UI polish in v1; basic responsive layout only.
- Theme system; one default theme.

## Acceptance Criteria

1. The web UI accepts a prompt and renders streamed tokens in real time.
2. SSE stream survives a slow tool call without dropping.
3. Admin endpoints require a valid token; missing token returns 401.
4. Audit log shows recent tool calls + permissions decisions with timestamps and
   identity.
5. The bundled frontend is ≤ 200 KiB minified and ≤ 50 KiB gzipped.
6. `tests/web/` ≥ 60% coverage (route handlers + auth + SSE wrapping).
7. Concurrent client count target: ≥ 20 active SSE streams on default settings.

## Design Doc Cross-References

- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
- [`../FRONTEND.md`](../FRONTEND.md)

## Risks

- cpp-httplib single-threaded event loop limits scale — v2 stretch to replace.
- SSE proxying through corporate proxies — document the fallback to long-poll.
- Token reuse across users — token rotation is operator-managed; document in
  `docs/RELIABILITY.md`.

## Validation

```sh
xmake build oran-web
xmake test test-web
xmake run orangutan-server -- --web --port 8080
curl -sN http://localhost:8080/chat/stream
```
