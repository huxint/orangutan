# Engineering Rules

Rules in this directory are enforced by `make ci` and code review. Keep this set minimal and factual.

| File | Scope |
| --- | --- |
| [`critical-rules.md`](critical-rules.md) | Non-negotiable C++26, async, RAII, header hygiene and security invariants. |
| [`code-style.md`](code-style.md) | Naming, formatting, C++26 idioms and reflection helpers. |
| [`docs-in-sync.md`](docs-in-sync.md) | Contract-to-code sync boundaries checked by `make ci`. |
| [`libraries.md`](libraries.md) | Approved C++ and frontend dependencies matching `xmake/packages.lua`. |
| [`prompt-design.md`](prompt-design.md) | Stable prompt prefix discipline and cache identity rules. |
| [`testing-and-bench.md`](testing-and-bench.md) | Catch2 tests, nanobench targets and opt-in live evaluations. |
