# Orangutan

C++26 Personal Agent runtime and local workbench. Prefer pure value transformations,
explicit Asio effect boundaries, RAII ownership, and verified tests. Preserve user
data; delete replaced code and stale documents.

## Workflow

1. Read [STATUS](docs/STATUS.md), [ROADMAP](docs/ROADMAP.md), and [ARCHITECTURE](docs/ARCHITECTURE.md).
2. Follow [critical rules](docs/rules/critical-rules.md) and [code style](docs/rules/code-style.md).
3. Verify affected behavior with `xmake build`, `xmake test`, `apps/desktop` checks, and `make ci`.
4. Keep owning contracts in sync ([docs-in-sync](docs/rules/docs-in-sync.md)).

## Contract Routing

| Work | Read |
| --- | --- |
| Desktop UI, local bridge & pets | [desktop-ui](docs/design-docs/desktop-ui.md) |
| Agent loop & context | [agent-platform](docs/design-docs/agent-platform.md), [agent-teams](docs/design-docs/agent-teams.md) |
| Runtime assembly & sessions | [bootstrap-runtime](docs/design-docs/bootstrap-runtime.md) |
| Tools, workspace & shell | [tool-runtime](docs/design-docs/tool-runtime.md) |
| Permissions & hooks | [permissions-and-hooks](docs/design-docs/permissions-and-hooks.md) |
| Session & long-term memory | [memory-system](docs/design-docs/memory-system.md) |
| Providers & prompt caching | [api-portability](docs/design-docs/api-portability.md), [prompt-design](docs/rules/prompt-design.md) |
| Messaging channels | [messaging-channels](docs/design-docs/messaging-channels.md) |
| Async, IO & SQLite | [async-model](docs/design-docs/async-model.md), [io-runtime](docs/design-docs/io-runtime.md), [storage-runtime](docs/design-docs/storage-runtime.md) |
| Configuration & secrets | [secrets-and-state](docs/design-docs/secrets-and-state.md) |
| Build, dependencies & tests | [BUILD_SYSTEM](docs/BUILD_SYSTEM.md), [libraries](docs/rules/libraries.md), [testing-and-bench](docs/rules/testing-and-bench.md) |

## Commands

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

Default build produces all 15 runtime libraries plus the `orangutan` Personal Agent CLI/server binary.
Use `xmake build test-<lib>` and `xmake run test-<lib>` while iterating.
