# Current Status

Orangutan is a **C++26 Personal Agent runtime and pure-Vulkan native desktop application**, built as 15 layered static libraries plus the default `orangutan` CLI executable, the opt-in pure-Vulkan native desktop (`orangutan-desktop`), and opt-in channel hosts (`oran-telegram`, `oran-qq-login`).

## Implemented Core

- **Runtime composition (`oran-bootstrap`)**: `RuntimeAssembly`, `HttpProviderBackend`, `AgentSession`, `BackgroundTasks` and dynamic Agent Teams over explicit Asio executors and SQLite pools.
- **Lean workspace & shell tools (`oran-tool`, `oran-io`)**: Minimal built-in surface (`FileRead`, `FileWrite`, `FileEdit`, plus bounded cancel-aware `ShellExec` over `io::run_subprocess`) behind pinned directory authority, path locks, permission evaluation, hooks and durable audit. Listing, searching and local CLI utilities compose directly through `ShellExec`.
- **Session & bilingual long-term memory (`oran-memory`, `oran-storage`)**: Atomic SQLite transcript + working-context checkpoint commits, budgeted context compaction, scoped FTS5 notes with CJK substring search support, revision-preserving updates and snapshot backup/import.
- **Dynamic subagents & teams (`oran-agent`, `oran-bootstrap`)**: One-generation bounded child runs, background jobs, dynamic specialist roles, reviewed dependency graphs and bounded peer messaging.
- **Pure-Vulkan Native Desktop (`apps/desktop-vk` → `orangutan-desktop`)**: Bespoke C++26 + Vulkan 1.0 SDF UI, FreeType2 CJK glyph atlas, and 100% procedural real-time 3D PBR raymarched **Forest Companions** (`Oran`, `Scout`, `Moss`, `Fern`, `Pebble`, `Pip`) directly hosting `oran-bootstrap` in-process with zero browser/web shell and zero external bitmap assets, featuring warm light/dark themes, task-splitting collaboration cards, interactive approval cards, SQLite memory & reminders, and headless Vulkan offscreen PNG capture (`--screenshot`).
- **Personal Agent CLI (`apps/personal` → `orangutan`)**: `orangutan chat` and `orangutan ask` with durable SQLite reminders and interactive terminal confirmations, supporting configured providers (`--config`) or zero-credential `--demo` exploration.
- **Messaging channels (`oran-channel`, `apps/telegram`, `apps/qq`)**: Deployable `oran-telegram` private host; `oran-qq-login` official QR binding + webhook verification/token refresh; Feishu adapter.

## Verification Gate

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
xmake build -j4 orangutan-desktop   # desktop Linux with Vulkan + SDL3 + FreeType2
build/linux/x86_64/release/orangutan-desktop --validate --showcase --screenshot /tmp/oran-vk.png
make ci
```
