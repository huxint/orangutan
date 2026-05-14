# Orangutan Legacy Audit

Reference distillation of what we learned from auditing the legacy `orangutan/` C++23
codebase (~40 kLoC). This file is what agents should consult to understand *why* the
v2 rewrite has the shape it does.

## Summary

The legacy code is **well-engineered but compile-time heavy**. It correctly modeled
the agent runtime domain (Message, Content, ToolDef, MemoryRecord), used modern C++23
patterns (deducing-this builders, std::expected, sender/receiver senders), and had
strong test coverage. Its weaknesses are concentrated in:

1. Compile time and build ergonomics.
2. Module modularity and parameter-object sprawl.
3. Single-platform channel monolith.
4. Mixed error-handling models (SQLite expected-vs-throwing).
5. Single-threaded web server.
6. Narrow hook surface.

Each is addressed in v2.

## Architecture Layers (Legacy)

```
main.cpp → bootstrap::run
   ├── cli (REPL + single-shot + slash commands)
   ├── web (cpp-httplib + SSE)
   ├── channel (QQ adapter, ~3.6 kLoC)
   ├── orchestration (mailbox + teammate runtime + team manager)
   ├── automation (cron + periodic + triggered)
   ├── agent (ReAct loop)
   │   ├── providers (transport + protocols + execution)
   │   ├── tools (registry + dispatch + permissions + hooks)
   │   ├── memory (store + runtime + search + mirror + age)
   │   ├── skills (loader)
   │   └── prompt (system prompt assembly)
   ├── config (JSON + secret encryption)
   ├── storage (SQLite + SessionStore)
   └── permissions (evaluator + rule-parser + safety-checks)
```

## Library Inventory (Legacy)

The legacy build packs everything into one static library `orangutan-lib`. v2 splits
this into ~22 libraries (see `docs/ARCHITECTURE.md`).

## Third-Party Dependencies (Legacy)

| Lib | v | Compile cost | v2 disposition |
| --- | --- | --- | --- |
| cli11 | 2.6.1 | low | kept |
| nlohmann_json | 3.12.0 | med | kept; use json_fwd in public headers |
| fmt | 12.1.0 | med | kept; header_only |
| spdlog | 1.17.0 | med | kept; behind `oran-log` shim |
| libcurl | 8.11.0 | med-high | kept |
| sqlite3 | 3.52.0 | med | kept; FTS5 retained |
| cpp-httplib | 0.37.2 | med | kept for v1 oran-web; v2 replacement stretch |
| stdexec-gtc | gtc-2026 | **high** | **removed**; replaced by asio + coroutines |
| rapidhash | 1.0 | low | kept |
| replxx | 2021.11.25 | low | kept |
| mbedtls | 3.6.1 | med-high | **removed**; libsodium for secrets; OpenSSL via curl for TLS |
| simdutf | 8.0.0 | med | kept |
| uni_algo | 1.2.0 | low-med | **removed**; folded into oran-core::str |
| ctre | 3.10.0 | med | **removed**; replaced by re2 |
| magic_enum | 0.9.7 | low | kept |
| Catch2 | 3.7.1 | med-high | kept |

## Top Friction List (Legacy → v2 Response)

1. **Compile times (~70 s)** — *no PCH / LTO / unity / modules; widely-included
   heavy headers.* → v2 mandates compile-budget rules ([`../rules/compile-budget.md`](../rules/compile-budget.md)),
   adopts PCH + modules, removes the heaviest libraries, and enforces include
   hygiene mechanically.

2. **stdexec sender complexity** — *type-erased `any_sender_of` was hard to debug and
   extend.* → v2 standardizes on **asio + coroutines** ([`../design-docs/async-model.md`](../design-docs/async-model.md)).

3. **SQLite throwing-vs-expected debt** — *~120 callsites mixing two error styles.* →
   v2 ships `oran-storage` **expected-only** from day one
   ([`../rules/error-handling.md`](../rules/error-handling.md)).

4. **File-vs-shell tool duplication** — *~2.2 kLoC of overlapping glob/ls/mkdir/mv
   logic.* → v2 splits cleanly: shell owns *raw* ops; file owns *structured* ops
   ([`../design-docs/tool-runtime.md#file-vs-shell-de-duplicated`](../design-docs/tool-runtime.md)).

5. **ToolRuntimeContext sprawl** — *8+ pointer parameter object.* → v2's
   `oran::tool::Runtime` is a typed handle with **capability-gated accessors**
   ([`../design-docs/tool-runtime.md#toolruntime`](../design-docs/tool-runtime.md)).

6. **QQ channel monolith** — *3.6 kLoC, no abstraction.* → v2 introduces the
   `Channel` trait + capability matrix; QQ becomes one optional library
   ([`../design-docs/channel-abstraction.md`](../design-docs/channel-abstraction.md)).

7. **Orchestration lease contention** — *per-agent mutex; no priority queue.* → v2
   adds bounded `async::Channel` mailbox with idempotency tags + priority hints
   ([`../design-docs/team-collaboration.md#mailbox`](../design-docs/team-collaboration.md)).

8. **Memory store single mutex** — *serialized all reads + writes.* → v2 uses WAL +
   connection pool ([`../design-docs/memory-system.md`](../design-docs/memory-system.md)).

9. **Per-JID serialization blocking** — *one slow response blocked the whole JID
   queue.* → v2 keeps per-conversation ordering but adds per-message deadlines.

10. **Permissions compile-time regex (ctre)** — *patterns hardcoded.* → v2 uses re2
    (runtime, config-driven).

11. **Hook subprocess overhead** — *one shell per hook event.* → v2 adds in-proc and
    (stretch) Lua/Wasm sinks; events become enumerable
    ([`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)).

12. **Config secrets no rotation** — *single password = all secrets.* → v2 adds
    `secrets rotate` subcommand, key versioning, plaintext zeroization
    ([`../design-docs/secrets-and-state.md`](../design-docs/secrets-and-state.md)).

13. **Web server single-threaded** — *~10-50 client cap.* → v2 keeps cpp-httplib for
    v1 deliverable, plans asio-based replacement for v2 stretch
    ([`../product-specs/0007-web-ui.md`](../product-specs/0007-web-ui.md)).

14. **Skills re-rendered every iteration** — *intentional, but unbounded cost.* → v2
    enforces a body-size cap per skill and caches inactive skills' metadata.

15. **Prompt cache string-concat key** — *no versioning, no invalidation.* → v2 uses
    `(section_id, content_hash, cache_version)` tuple
    ([`../design-docs/api-portability.md#cache-key-versioning`](../design-docs/api-portability.md)).

## What We Preserve

The following pieces from the legacy code earn their place in v2:

- The **ReAct loop shape** (build prompt → provider send → parse tool calls →
  dispatch → loop). It's the right abstraction.
- The **layered provider architecture** (transport → protocol → execution).
- The **fluent builder pattern** using C++23 deducing-this for configurable types.
- `utils::all_ok` for `Result` combination.
- `utils::Overloaded` for variant visitation.
- `utils::enum_name` wrapping magic_enum.
- The **session-store row-mapper** approach (`sqlite::read_columns<Ts...>`).
- The **identity / scope_key** derivation for namespacing memory.
- The **automation engine's executor-callback pattern** (build a fresh runtime per
  firing).
- The **approval-prompt signing** mechanism for permission `ask` flow.
- The **MEMORY.md mirror** as a human-inspection surface.

## What Lives Where In Legacy (Quick Map)

| Concern              | Legacy path                                          |
| -------------------- | ---------------------------------------------------- |
| Entry point          | `orangutan/src/main.cpp` + `bootstrap/bootstrap.cpp` |
| ReAct loop           | `orangutan/src/agent/agent-loop.{hpp,cpp}`           |
| Providers            | `orangutan/src/providers/`                           |
| Tools                | `orangutan/src/tools/`                               |
| Memory               | `orangutan/src/memory/`                              |
| Orchestration        | `orangutan/src/orchestration/`                       |
| Automation           | `orangutan/src/automation/`                          |
| Channels             | `orangutan/src/channel/` (QQ in `channel/qq/`)       |
| Web                  | `orangutan/src/web/`                                 |
| CLI                  | `orangutan/src/cli/`                                 |
| Config + secrets     | `orangutan/src/config/`                              |
| Storage              | `orangutan/src/storage/`                             |
| Hooks                | `orangutan/src/hooks/`                               |
| Permissions          | `orangutan/src/permissions/`                         |
| Utilities            | `orangutan/src/utils/`                               |

## When To Read The Legacy Code

- You're about to write a v2 module and want to understand the requirement that
  shaped its v1 ancestor.
- You're porting a concrete artifact (QQ adapter, session store, agent loop) and
  want the behavior reference.
- You're debugging a regression and need to compare against a known-working v1 path.

## When To NOT Copy The Legacy Code

- Anywhere the audit lists it as "removed" or "replaced".
- Anywhere the v2 rule explicitly differs (e.g., expected-only error handling).
- Anywhere it pulls in a removed third-party (stdexec, mbedtls, ctre, uni_algo).
- File-by-file copying without re-applying the architectural and rule changes.

## See Also

- [`harness-template-distill.md`](harness-template-distill.md) — the prompt
  framework philosophy we adopt.
- [`third-party-libs.md`](third-party-libs.md) — library-specific notes.
