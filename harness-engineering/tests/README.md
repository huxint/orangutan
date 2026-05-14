# `tests/` — Catch2 Test Buckets

One subdirectory per library: `tests/<lib>/`. Each bucket is an independent xmake
target so iterating on `tests/<lib>/` does not recompile other buckets.

## Layout

```
tests/
├── test-helpers/         # shared helpers: unique paths, mocks, fakes
│   ├── unique-paths.hpp
│   ├── scoped-env.hpp
│   ├── fake-provider.hpp
│   └── ...
├── core/
├── async/
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
├── channel-qq/
├── web/
├── cli/
├── bootstrap/
└── integration/          # end-to-end paths with mocked externals
```

## Conventions

- Catch2 v3. `[unit]` / `[integration]` / `[property]` tags.
- One file per scenario / feature; small. Don't create monoliths.
- Use `test-helpers/` for shared setup; do not duplicate fixtures.
- See [`../docs/rules/testing-and-bench.md`](../docs/rules/testing-and-bench.md) for
  the full convention.

## Running

```sh
xmake test                    # all buckets
xmake run test-agent          # one bucket
xmake run test-agent "[unit]" # tag filter
xmake run test-agent "specific test name"
```

## Status

Empty. First buckets land with the MVP code per
[`../docs/product-specs/0001-core-react-loop.md`](../docs/product-specs/0001-core-react-loop.md).
