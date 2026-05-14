# `bench/` — Benchmark Buckets

`bench/` is **first-class** alongside `tests/`. Every library has both.

## Layout

```
bench/
├── bench-helpers/          # shared corpus generators, mock IO, etc.
├── core/
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
├── automation/
├── channel/
├── compile-time/           # per-TU compile-time scenarios
└── README.md               # this file
```

## Conventions

- nanobench is the default runner; Catch2's `BENCHMARK` macro is also accepted.
- Each bucket has:
  - `bench/<lib>/main.cpp` — entry point.
  - `bench/<lib>/scenarios/<scenario>.cpp` — one file per scenario.
  - `bench/<lib>/README.md` — describes what scenarios exist and what they compare.
- Each meaningful design tradeoff ships an **A-vs-B** comparison.
- Output: machine-readable JSON to stdout when `--json` flag is set.
- See [`../docs/rules/testing-and-bench.md`](../docs/rules/testing-and-bench.md) and
  [`../docs/product-specs/0010-benchmark-harness.md`](../docs/product-specs/0010-benchmark-harness.md).

## Running

```sh
# Build one bucket and run
xmake build bench-memory && xmake run bench-memory --json > out.json

# Compare against baseline
scripts/bench-compare.sh memory

# Build the unified bench binary
xmake build orangutan-bench
xmake run orangutan-bench --json > all.json
```

## A-vs-B Spotlight

| Bucket             | A                              | B                          |
| ------------------ | ------------------------------ | -------------------------- |
| `core`             | variant-based `Content`        | polymorphic `Content` (stretch) |
| `async`            | `Awaitable<T>` pipeline        | callback-based pipeline    |
| `memory`           | FTS5 backend                   | sqlite-vec backend (v2)    |
| `provider`         | nlohmann_json encode/decode    | simdjson (stretch)         |
| `orchestration`    | leader-worker strategy         | vote strategy              |
| `tool`             | hashmap registry lookup        | static dispatch (stretch)  |
| `channel`          | bounded `Channel<T>`           | unbounded queue            |

Each table row corresponds to at least one scenario file in the bucket.

## Status

Empty. First scenarios land with the MVP code.
