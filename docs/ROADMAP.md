# Roadmap

## Completed Foundations

| Milestone | Delivered |
| --- | --- |
| Minimal C++26 runtime | 15 layered static libraries, pure policy/prompt core, explicit Asio/HTTP/IO/SQLite boundaries. |
| Workspace, shell & permissions | Pinned `DirectoryAuthority`, minimal `FileRead`/`Write`/`Edit`, cancel-aware `ShellExec`, allow/ask/deny rules, hooks and audit. |
| Persistent memory & continuity | Atomic session + checkpoint commits, context compaction, scoped FTS5 + CJK search, backup/import recovery. |
| Collaboration & teams | Bounded child runs, background jobs, dynamic specialists, reviewed DAG dependencies and peer messages. |
| Pure-Vulkan desktop & CLI | Native C++26 `orangutan-desktop` (pure Vulkan 1.0 SDF + real-time 3D PBR raymarched Forest Companions) and `orangutan` CLI (`chat`, `ask`) with SQLite reminders and live approvals. |

## Open Follow-Ups

| Area | Next Slice |
| --- | --- |
| **Network & web tools** | Add permissioned `WebFetch` / `WebSearch` over `oran-http` (`Capability::egress_http`) with SSRF protection and size bounds. |
| **Memory & context quality** | Run deployment-model evaluations (`eval-context`, unhinted recall/correction cases), add note provenance and curb summary drift. |
| **Durable background recovery** | Persist in-flight background jobs, team boards and completion outboxes across host restarts with interrupted-run settlement. |
| **Multi-channel deployment** | Compose official QQ QR binding + webhook/outbox into a restart-safe `oran-qq` host and complete Feishu authenticated ingress. |
| **Desktop system integration** | Add desktop notifications, system tray presence, and cross-platform packaging (AppImage/Flatpak) for `orangutan-desktop`. |
