# 0005 — Memory System

## User Problem

Agents that forget everything between turns are useless beyond a single conversation.
The legacy `orangutan/` had a single-tier memory; v2 makes the tiers explicit so
operators can reason about retention, scope, and visibility.

## Scope (v1)

- `oran-memory::session::Store` — per-session conversation history (replaces legacy
  `SessionStore`), expected-only API, prepared-statement cache, WAL.
- `oran-memory::longterm::Runtime` with `Fts5Backend` (default).
- `MemoryRecord` kinds: `user`, `feedback`, `project`, `reference`.
- Decay policy applied by a periodic job (`oran-automation`).
- Optional `MEMORY.md` mirror under `<workspace>/.orangutan/memory/`.
- Hook events on read / write / forget / decay.

## Scope (v1.1)

- `Memory::team::Store` — shared tier for `oran-orchestration` teams.
- `kind = team` for cross-tier search visibility.
- Approval signing replay-safe across rotations.

## Scope (v2)

- `VectorBackend` (optional, gated `--vector_memory=y`).
- Hybrid search (FTS5 score + cosine).
- Externalized embedding store via `oran-http::Client`.

## Out Of Scope

- Cross-runtime sync without explicit `scope_pin`.
- Memory replication.

## Acceptance Criteria

1. A `SessionStore::append(...)` followed by `SessionStore::load(...)` round-trips
   500 messages without loss.
2. `longterm::Runtime::search("react agent loop", limit=10)` returns within 50 ms on
   a 10 k-record corpus.
3. The MEMORY.md mirror, when enabled, reflects all kinds + records within 1 s of the
   underlying DB write.
4. Decay marks records older than `policy.forget_after_unused` as shadow; they no
   longer surface in default search.
5. A `memory.write.before` hook can veto a write; the runtime returns
   `Error::HookVeto` to the caller.
6. `tests/memory/` ≥ 85% coverage.
7. `bench/memory/search-fts5-vs-vector` (v2): reports the FTS5 baseline + vector
   results in machine-readable JSON.

## Design Doc Cross-References

- [`../design-docs/memory-system.md`](../design-docs/memory-system.md)
- [`../design-docs/secrets-and-state.md`](../design-docs/secrets-and-state.md)
- [`../design-docs/team-collaboration.md`](../design-docs/team-collaboration.md)
  (shared tier)

## Risks

- WAL contention if `sessions.db` and `memory.db` are not split — they are. Verify
  with a bench under multi-conversation load.
- FTS5 ranking quality on Chinese / Japanese / Korean text — track in the
  tech-debt-tracker; consider `simdjson`-style tokenizer or vector for non-Latin
  corpora in v2.

## Validation

```sh
xmake build oran-memory
xmake test test-memory
scripts/bench-compare.sh memory
```
