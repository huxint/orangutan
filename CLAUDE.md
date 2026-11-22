# Orangutan

C++26 agent runtime. Build the smallest complete provider/tool/memory loop, then
extend it through the same contracts. Prefer value transformations and explicit
effect boundaries. Preserve user data; delete replaced code and stale documents.

## Workflow

1. Read [STATUS](docs/STATUS.md), [ROADMAP](docs/ROADMAP.md) and the
   [architecture](docs/ARCHITECTURE.md), then the relevant contract below.
2. Scope a complete slice. Large changes need a checked-in
   [plan](docs/PLANS_GUIDE.md) before implementation.
3. Follow [critical rules](docs/rules/critical-rules.md) and the
   [compile budget](docs/rules/compile-budget.md).
4. Verify affected behavior, run its build/tests and `make ci`, then commit.
5. Update the document that owns any changed contract. Git owns history;
   [docs-in-sync](docs/rules/docs-in-sync.md) defines the rule.

## Contract Routing

| Work | Read |
| --- | --- |
| Desktop UI and host integration | [desktop-ui](docs/design-docs/desktop-ui.md), [desktop product](docs/product-specs/desktop-prototype.md) |
| Agent execution and context | [agent-platform](docs/design-docs/agent-platform.md) |
| Runtime composition and session hosting | [bootstrap-runtime](docs/design-docs/bootstrap-runtime.md) |
| Tool dispatch and filesystem policy | [tool-runtime](docs/design-docs/tool-runtime.md) |
| Permission decisions and approvals | [permissions-and-hooks](docs/design-docs/permissions-and-hooks.md) |
| Session and long-term memory | [memory-system](docs/design-docs/memory-system.md) |
| Providers and prompt caching | [api-portability](docs/design-docs/api-portability.md), [prompt-design](docs/rules/prompt-design.md) |
| Async ownership and cancellation | [async-model](docs/design-docs/async-model.md), [async rules](docs/rules/async-and-concurrency.md) |
| Filesystem and SQLite | [io-runtime](docs/design-docs/io-runtime.md), [storage-runtime](docs/design-docs/storage-runtime.md) |
| Configuration and state | [secrets-and-state](docs/design-docs/secrets-and-state.md) |
| Build and dependencies | [BUILD_SYSTEM](docs/BUILD_SYSTEM.md), [libraries](docs/rules/libraries.md) |
| Tests and benchmarks | [testing-and-bench](docs/rules/testing-and-bench.md) |
| Commits and reviews | [REPO_COLLAB_GUIDE](docs/REPO_COLLAB_GUIDE.md), [workflow](docs/rules/workflow.md) |

Runtime prompt bytes follow `prompt-design.md`; this file routes development work.

## Commands

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

`make ci` checks repository contracts and hygiene; it does not build C++.
Use `xmake build test-<lib>` and `xmake run test-<lib>` while iterating.
