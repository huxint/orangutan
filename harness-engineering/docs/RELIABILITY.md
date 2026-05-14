# Reliability

The runtime is meant to be **boring to operate**: start it, observe it, restart it,
trust the audit log. This doc captures the operational expectations.

## Startup And Health

- `orangutan` checks for a stale `<workspace>/.orangutan/lock` file on start; if the
  PID isn't live, removes it; otherwise refuses to start.
- Health endpoint (web mode): `GET /healthz` returns `{ "status": "ok", "uptime_s":
  N, "agents": [...] }`.
- Liveness for systemd: `Type=notify` + `sd_notify(READY=1)` once the runtime's
  executors are running.

## Logging

- spdlog through the `oran-log` shim.
- Default level: `info` (console), `debug` (file).
- File sink: `<workspace>/.orangutan/logs/orangutan-YYYY-MM-DD.log`, daily rotation,
  7-day retention. Configurable.
- Structured fields preferred over composed messages: `log::info("tool dispatched",
  field("tool", name), field("ms", duration_ms))`.

## Metrics And Tracing

- v1: counters exposed via `GET /metrics` in Prometheus exposition format from the web
  layer.
- v1.1: distributed tracing via OpenTelemetry (stretch — adds a dependency; gate it
  behind `--obs_otel=y`).

## Required Environment

| Variable                  | Required? | Default | Purpose |
| ------------------------- | --------- | ------- | ------- |
| `ANTHROPIC_API_KEY`       | One of    | —       | Anthropic provider creds (referenced from config). |
| `OPENAI_API_KEY`          | One of    | —       | OpenAI provider creds. |
| `ORAN_SECRET_PASSWORD`    | Recommended | —     | Decrypts secret-protected config fields. |
| `ORAN_WORKSPACE`          | No         | `cwd` | Workspace path. |
| `ORAN_TEST_REAL_PROVIDERS` | No (tests only) | unset | Enable real-provider integration tests. |

## Retries / Backoff

- Provider retries on `network`, `rate_limit`, `upstream`. Backoff:
  exponential with jitter; cap 30 s; max attempts 5 (config: `provider.retry`).
- Fallback model switch on retryable failure of primary after the configured
  attempt count.
- Channel adapters retry on transient HTTP errors with the same shape.

## Timeouts

- Provider request: `provider.request_timeout_seconds` (default 90).
- Tool execution: per-tool `timeout_seconds` (default 60).
- Hook execution: `hooks.timeout_ms` (default 2000).
- Channel inbound: `channel.<id>.message_deadline_seconds` (default 300).

## Cancellation

- SIGINT / SIGTERM signals the runtime's root cancellation_signal.
- Runtime waits up to `shutdown_grace_seconds` (default 10) for in-flight work,
  then force-closes channels and exits.

## Backups

- SQLite databases are routine backup targets:
  `<workspace>/.orangutan/{sessions,memory,automation,audit}.db`.
- `scripts/backup-db.sh` (TBD) snapshots all four with `sqlite3 .backup`.
- Recommended cadence depends on operator workload; document in your deployment
  runbook.

## Incident Notes

A `docs/histories/YYYY-MM/incidents-*.md` slot is appropriate for postmortems of
production incidents. Keep them short, technical, and blameless.

## Failure Modes

| Failure                       | Behavior |
| ----------------------------- | -------- |
| Disk full                     | Storage layer returns `Error::storage`; agent surfaces error to user. Audit log entry. |
| Out of memory                 | OS kills process; supervisor restarts. |
| Provider 5xx storm            | Retry + fallback model. Hook `provider.fallback` fires. |
| Channel API rate-limit        | Adapter pauses inbound, surfaces hook `channel.rate_limited` (planned). |
| Mailbox overflow              | `try_send` returns `MailboxOverflowed`; team strategy may divert / drop. |
| Hook timeout (blocking sink)  | Triggering action denied; logged at WARN. |

## See Also

- [`SECURITY.md`](SECURITY.md)
- [`CICD.md`](CICD.md)
- [`design-docs/async-model.md`](design-docs/async-model.md) (cancellation,
  backpressure)
