# Frontend Guide

Read this file when working on the web UI surface (`oran-web`). It covers local dev,
build, conventions, and testing strategy.

## Current State

- **Server**: `oran-web` (cpp-httplib in v1; asio replacement is stretch).
- **Frontend**: a single-page app under `web/frontend/` (TBD with first web-UI PR).
- **Bundle target**: ≤ 200 KiB minified, ≤ 50 KiB gzipped.

## Local Dev

```sh
# Run the server with web mode enabled
xmake build orangutan
xmake run orangutan -- --web --port 8080

# Open the UI
$BROWSER http://localhost:8080
```

For frontend-only iteration (TBD), a small `vite` dev server proxies API calls to
the running `orangutan-server`.

## Conventions

- **Single source of truth** for shared types between server and client: a generated
  TypeScript file under `docs/generated/web-types.d.ts`, produced from the C++ route
  handlers via a `scripts/gen-web-types.sh` helper.
- **CSS**: utility-first; minimal custom CSS.
- **JS framework**: Preact (small footprint) for v1; switch to "no framework" if the
  app stays simple.
- **SSE**: every streaming response uses Server-Sent Events with the
  `text/event-stream` content type and the standard event/data structure.
- **Token auth**: `Authorization: Bearer <token>` header; missing token returns 401.

## Testing

- Backend: `tests/web/` exercises route handlers, auth, SSE wrapping with a
  cpp-httplib client.
- Frontend: pick one of {Playwright, Cypress} once UI shape stabilizes; capture
  the choice in `docs/exec-plans/active/` when introducing it.
- Visual regression: out of scope for v1.

## See Also

- [`product-specs/0007-web-ui.md`](product-specs/0007-web-ui.md)
- [`design-docs/agent-platform.md`](design-docs/agent-platform.md)
- [`RELIABILITY.md`](RELIABILITY.md) (`GET /healthz`, `GET /metrics`)
