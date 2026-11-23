# Roadmap

## Completed Foundations

| Milestone | Delivered |
| --- | --- |
| Minimal C++26 runtime | 15 layered static libraries, pure policy/prompt core, explicit Asio/HTTP/IO/SQLite boundaries. |
| Workspace, shell & permissions | Pinned `DirectoryAuthority`, `FileRead`/`Write`/`Edit`/`List`/`Search`, cancel-aware `ShellExec`, allow/ask/deny rules, hooks and audit. |
| Persistent memory & continuity | Atomic session + checkpoint commits, context compaction, scoped FTS5 + CJK search, backup/import recovery. |
| Collaboration & teams | Bounded child runs, background jobs, dynamic specialists, reviewed DAG dependencies and peer messages. |
| Personal Agent host & workbench | Unified `orangutan` CLI + loopback HTTP/SSE server, SQLite reminders, live interactive approvals and Desktop UI with Forest Companions. |

## Open Follow-Ups

| Area | Next Slice |
| --- | --- |
| **Network & web tools** | Add permissioned `WebFetch` / `WebSearch` over `oran-http` (`Capability::egress_http`) with SSRF protection and size bounds. |
| **Memory & context quality** | Run deployment-model evaluations (`eval-context`, unhinted recall/correction cases), add note provenance and curb summary drift. |
| **Durable background recovery** | Persist in-flight background jobs, team boards and completion outboxes across host restarts with interrupted-run settlement. |
| **Multi-channel deployment** | Compose official QQ QR binding + webhook/outbox into a restart-safe `oran-qq` host and complete Feishu authenticated ingress. |
| **Native desktop packaging** | Package `orangutan serve` + `apps/desktop` into a single local desktop app with OS notifications and stop-on-close lifecycle. |
