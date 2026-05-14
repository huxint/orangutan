# Testing And Benchmarking

Tests and benchmarks are **first-class peers**. Every library has both. This file
documents the layout, frameworks, and expectations.

## Layout

```
tests/
├── core/                 Catch2 bucket for oran-core
├── async/                Catch2 bucket for oran-async
├── log/
├── io/
├── http/
├── storage/
├── config/
├── permission/
├── skill/
├── hook/
├── tool/
├── memory/
├── provider/
├── prompt/
├── agent/
├── orchestration/
├── automation/
├── channel/
├── channel-qq/           (optional, gated by channel_qq)
├── web/
├── cli/
├── bootstrap/
├── integration/          end-to-end tests
└── test-helpers/         shared helpers: unique paths, fake providers, mock channels
```

```
bench/
├── core/                 nanobench/Catch2 bucket
├── async/
├── http/
├── storage/
├── permission/
├── tool/
├── memory/
├── provider/
├── prompt/
├── agent/
├── orchestration/
├── channel/
├── compile-time/         per-TU compile-time measurements
└── bench-helpers/        shared helpers
```

## Tests

### Framework: Catch2 v3

- Tag every test with a category: `[unit]`, `[integration]`, `[property]`.
- Test case names are imperative: `TEST_CASE("loop completes a single-iteration turn",
  "[unit]")`.
- Use `SECTION` for sub-scenarios that share setup.

### Test Helpers (`tests/test-helpers/`)

Carry over from legacy:

- `unique_test_path()` — gives a temp path namespaced by test name.
- `unique_test_db_path()` — for SQLite tests.
- `ScopedEnvVar` — RAII env var.
- `FakeProviderBackend` — captures requests, returns canned responses.
- `make_test_route(...)` — builds a provider Route for tests.
- `mock_channel::*` — minimal channel adapter for orchestration tests.

### Coverage Expectations

| Library category | Min coverage |
| --- | --- |
| Foundation (core, async, log, io)         | 90% |
| Storage / config / permission             | 85% |
| Tool / memory / hook / skill              | 80% |
| Provider / prompt / agent                  | 75% |
| Orchestration / automation                 | 70% |
| Channel adapters                            | 60% (network paths mocked) |
| Web                                         | 60% |
| CLI                                         | 50% |

Coverage tracked via `xmake check coverage` (or `llvm-cov` when using Clang).
Reported in `docs/generated/coverage-YYYY-MM-DD.json`.

### What To Test

- Happy path.
- One representative failure per error category.
- Cancellation behavior (every async function has at least one "cancelled mid-flight"
  test).
- Permission gating (one allow, one deny, one ask per gated operation).
- Hook firing order (one test per blocking-vs-advisory).

### Integration Tests

`tests/integration/` exercises end-to-end paths:

- Bootstrap → agent loop → mock provider → mock tool → response.
- Bootstrap → web HTTP → SSE stream → response.
- Bootstrap → automation tick → agent run.

Integration tests use **mock providers** by default. Real-provider tests run in a
nightly job (gated by env var `ORAN_TEST_REAL_PROVIDERS=1`).

## Benchmarks

### Framework: nanobench

- Simple, low-overhead microbenchmark library.
- Catch2's `BENCHMARK` macro is available too (it wraps nanobench).
- Output: human-readable + machine-readable JSON.

### Bench Structure

Each bench bucket has at least:

- `bench/<lib>/main.cpp` — entry point (registers benches with the runner).
- `bench/<lib>/scenarios/<scenario>.cpp` — one scenario per file.
- `bench/<lib>/README.md` — describes what scenarios exist and what they compare.

### A-vs-B Pattern (Core Convention)

When a design choice has plausible alternatives, the bench compares them:

```cpp
// bench/memory/scenarios/search-fts5-vs-vector.cpp
TEST_CASE("memory.search: FTS5 baseline vs vector", "[bench]") {
  auto corpus = make_test_corpus(/* records= */ 10'000);

  BENCHMARK_ADVANCED("fts5 baseline")(Catch::Benchmark::Chronometer m) {
    auto store = make_fts5_store(corpus);
    m.measure([&] { return store.search("react agent loop", /*limit=*/10); });
  };

  BENCHMARK_ADVANCED("vector cosine")(Catch::Benchmark::Chronometer m) {
    auto store = make_vector_store(corpus);
    m.measure([&] { return store.search("react agent loop", /*limit=*/10); });
  };
}
```

Each scenario emits one line of nanobench output; `compare.cpp` runs all scenarios
and prints a comparison summary.

### Bench Categories

| Category | Library | Compares |
| --- | --- | --- |
| Encoding overhead | provider | nlohmann_json vs. simdjson |
| Search backend | memory | FTS5 vs. sqlite-vec vs. external HTTP API |
| Dispatch overhead | tool | static lookup vs. hashmap |
| Permission eval | permission | rule-tree vs. linear scan |
| Mailbox throughput | orchestration | bounded channel vs. asio::channel native |
| Strategy cost | orchestration | leader-worker vs. vote |
| Coroutine vs callback | async | awaitable<T> vs. callback-based |
| Compile time | compile-time | pimpl vs. inline private members |

### Bench Output Format

```json
{
  "version": 1,
  "ran_at": "2026-05-14T10:23:42Z",
  "machine": { "cpus": 8, "ram_gb": 16, "model": "..." },
  "results": [
    {
      "bench": "memory.search.fts5_baseline",
      "iters": 1000,
      "median_ns": 124000,
      "p95_ns":    180000,
      "p99_ns":    250000
    },
    ...
  ]
}
```

Stored under `docs/generated/bench-YYYY-MM-DD.json`. CI's nightly job runs the
benchmark suite and updates the file via PR.

### Comparison Runner

`scripts/bench-compare.sh <library>`:

- Builds `bench-<library>` if needed.
- Runs it.
- Loads previous baseline from `docs/generated/bench-baseline-<library>.json`.
- Prints a table: scenario | median | delta vs. baseline.
- Exits non-zero if any scenario regressed > 10%.

## CI Wiring

- `make ci` (docs + hygiene + shell-lint) on every PR.
- `xmake test` on every PR.
- `xmake build bench-*` smoke-built on every PR (compile-only; does not run benches).
- **Nightly**: full bench run, compile-budget recheck, real-provider integration
  (gated).
- **Pre-merge**: maintainer manually triggers a bench compare for `perf-impact` PRs.

## Expectations Per Change

| Change type | Tests | Benches |
| --- | --- | --- |
| Bug fix | regression test | (none unless perf bug) |
| New feature | tests for happy path + failures + cancellation | one bench if perf is plausibly affected |
| Refactor | existing tests pass | bench delta documented if any |
| Perf improvement | tests pass | mandatory bench A/B; commit message includes numbers |
| Build / packaging | build & test pass | (none) |
| Doc-only | (none) | (none) |

## Anti-Patterns

- "We'll add benches later." Add one at submission time, even if minimal.
- Benches that don't compare anything. The "A-vs-B" pattern keeps benches meaningful.
- Tests that depend on real network calls without an opt-in env var.
- Tests that use `std::this_thread::sleep_for` to "wait for async to finish". Use
  `tests/async/run_one()` or `run_with_timeout()`.

## See Also

- [`compile-budget.md`](compile-budget.md) — compile-time is also benched.
- [`../product-specs/0010-benchmark-harness.md`](../product-specs/0010-benchmark-harness.md)
  — concrete v1 deliverable.
