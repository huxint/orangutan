# Compile Budget

The legacy `orangutan/` exceeded 70 s clean and minutes incremental on 16 GB RAM. We
budget per translation unit and per build phase so the project stays in the < 5 s
incremental band.

## Reference Hardware

Budgets are stated for **GCC 16.1, x86_64 Linux, 8 cores, 16 GB RAM, NVMe SSD,
release mode (LTO on), -j$(nproc)**. CI runs this baseline and a slower one (4 cores)
for sanity.

## Per-TU Budgets

| Library category                  | Median   | p95     | Hard cap |
| --------------------------------- | -------- | ------- | -------- |
| `oran-core`, `oran-log`, `oran-io` | 0.8 s   | 1.5 s   | 2.0 s    |
| `oran-async`                       | 1.0 s   | 2.0 s   | 2.5 s    |
| `oran-storage`, `oran-config`      | 1.0 s   | 2.0 s   | 2.5 s    |
| `oran-permission`, `oran-skill`    | 1.0 s   | 2.0 s   | 2.5 s    |
| `oran-tool`, `oran-memory`, `oran-hook` | 1.2 s | 2.5 s | 3.0 s    |
| `oran-provider`, `oran-prompt`     | 1.5 s   | 3.0 s   | 3.5 s    |
| `oran-agent`                       | 1.5 s   | 3.0 s   | 3.5 s    |
| `oran-orchestration`, `oran-automation` | 1.5 s | 3.0 s | 3.5 s   |
| `oran-channel`, `oran-channel-*`   | 1.2 s   | 2.5 s   | 3.0 s    |
| `oran-web`                          | 1.5 s   | 3.0 s   | 4.0 s    |
| `oran-cli`                          | 1.5 s   | 3.0 s   | 4.0 s    |
| `oran-bootstrap`                    | 2.0 s   | 4.0 s   | 5.0 s    |

A TU exceeding hard cap **fails CI**. Median / p95 regressions trigger a warning.

## Per-Target Budgets

| Phase                                          | Target    | Hard cap |
| ---------------------------------------------- | --------- | -------- |
| Configure (`xmake f`)                           | 5 s       | 20 s     |
| Build all libs (`xmake build oran-*`)           | 25 s      | 45 s     |
| Build all binaries (`xmake build orangutan*`)    | 30 s      | 55 s     |
| Link `orangutan` binary                          | 5 s       | 10 s     |
| Total clean build (`xmake -j$(nproc)`)          | 30 s      | 60 s     |
| Incremental rebuild after one `.cpp` edit       | 3 s       | 10 s     |
| Incremental rebuild after one public header edit | 8 s       | 20 s     |
| Tests build (`xmake build test-*`)              | +15 s     | +30 s    |
| Bench build (`xmake build bench-*`)             | +10 s     | +20 s    |

Hard-cap exceedance fails CI.

## Memory Budget

Compile-time peak RSS per cc1plus invocation is also tracked. We aim for ≤ 1 GB on
the heaviest TUs (typically `oran-agent::Loop::run`'s impl). RAM exhaustion is what
made the legacy project unbuildable on 16 GB; we will not regress.

| TU category | Median RSS | p95 RSS | Hard cap |
| --- | --- | --- | --- |
| Core libs   | 250 MB     | 400 MB  | 600 MB   |
| Mid libs    | 350 MB     | 600 MB  | 900 MB   |
| Agent / orchestration / bootstrap | 450 MB | 800 MB | 1.2 GB |

## Enforcement

### Mechanical Checks

`scripts/check-compile-budget.sh` (build skeleton, to be implemented):

```sh
xmake clean
xmake f -m release
TIMEFORMAT='%R'
{ time xmake -j$(nproc); } 2>build.time
xmake compile_commands ;# regenerates compile_commands.json

scripts/measure-tu.sh --json > tu-times.json
python3 scripts/check-tu-budget.py tu-times.json compile_budget.json
```

`compile_budget.json` is a versioned baseline. Entries:

```jsonc
{
  "categories": {
    "oran-core":    { "median": 0.8, "p95": 1.5, "hard_cap": 2.0 },
    "oran-tool":    { "median": 1.2, "p95": 2.5, "hard_cap": 3.0 },
    ...
  },
  "targets": {
    "clean_build_release": { "target": 30, "hard_cap": 60 },
    ...
  }
}
```

### CI

CI runs `check-compile-budget.sh` on every PR. Outcomes:

- All within median / p95 → green.
- Within hard cap but > p95 → yellow (warning comment on the PR).
- Beyond hard cap → red (fails the build).

### Baseline Updates

When a planned change legitimately raises the budget (e.g., adding a new library):

1. Open an exec plan describing the increase.
2. PR includes the `compile_budget.json` bump alongside the code change.
3. Reviewer signs off on both code and budget.

## Per-PR Self-Check

```sh
# Before opening a PR:
make ci                          # docs + hygiene
xmake f -m release && xmake      # build
xmake test                       # all tests
scripts/check-compile-budget.sh  # the budget
```

The PR template's "Validation" section includes a checkbox for budget compliance.

## When You Hit The Budget

Order of attack:

1. **Forward-decl in the public header.** Look at `#include`s in `include/oran/<lib>/`.
2. **Pimpl heavy classes.** Anything owning asio / json / sqlite handles.
3. **Move template definitions out of headers** (use `extern template` if it's heavily
   instantiated).
4. **Split the TU.** A huge `.cpp` can be split into multiple smaller ones; the linker
   handles the rest.
5. **Reach for modules.** When the surface is stable, convert to a module unit.
6. **Unity build only as a last resort** — and only for cold libraries.

Always measure with `scripts/measure-tu.sh` *before* and *after*. Record the delta in
the PR description.

## Anti-Patterns

- Disabling LTO "for now" to fit the budget. Open an exec plan first.
- Inflating the budget instead of fixing the cause. Allowed but rare; PR must say so
  explicitly.
- Comparing on different hardware. CI's hardware is the source of truth.
- Marking a hard-cap failure as flaky. Compile time is not flaky; investigate.

## See Also

- [`../FAST_COMPILATION.md`](../FAST_COMPILATION.md) — the playbook.
- [`module-and-pch.md`](module-and-pch.md) — module/PCH mechanics.
- [`../design-docs/module-boundaries.md`](../design-docs/module-boundaries.md) — why
  the boundaries exist.
