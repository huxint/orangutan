# `skeleton/` — Starter Project Skeleton

This folder contains a **starter** for the first xmake build of Orangutan v2. It is
deliberately minimal: enough to demonstrate the build wiring described in
[`../docs/BUILD_SYSTEM.md`](../docs/BUILD_SYSTEM.md), not enough to run an agent.

The intent: an agent that has read `AGENTS.md` + `docs/ARCHITECTURE.md` +
`docs/BUILD_SYSTEM.md` can copy these files to the project root, edit a few
placeholders, and have a building project skeleton in under five minutes.

## Layout

```
skeleton/
├── xmake/
│   ├── options.lua         # all option() declarations
│   ├── toolchain.lua       # oran-gcc toolchain (GCC 16.1)
│   ├── packages.lua        # all add_requires()
│   ├── targets.lua         # oran_lib() helper + library list
│   ├── tests.lua           # tests/<lib>/ buckets
│   ├── bench.lua           # bench/<lib>/ buckets
│   └── checks.lua          # CI-friendly checks
├── xmake.lua               # project-wide settings
├── include/
│   └── oran/
│       ├── _pch.hpp        # the PCH (stable headers only)
│       ├── core/
│       │   ├── error.hpp
│       │   ├── result.hpp
│       │   └── ...
│       └── async/
│           └── awaitable_fwd.hpp
└── src/
    ├── main.cpp            # boots oran::bootstrap::run
    └── oran-core/
        └── error.cpp       # smallest viable implementation
```

## How To Use

1. Read [`../docs/BUILD_SYSTEM.md`](../docs/BUILD_SYSTEM.md) for the rationale.
2. Copy `skeleton/xmake.lua` and `skeleton/xmake/` into the project root.
3. Copy `skeleton/include/` and `skeleton/src/` into the project root.
4. `xmake f -m release && xmake build orangutan` should succeed (smoke).
5. Open the first execution plan: `make new-plan SLUG=mvp-react-loop`.

## Status

**To be populated.** The skeleton ships in the same PR that lands the first xmake
build. Until then, this README documents the intent. Track via
[`../docs/exec-plans/tech-debt-tracker.md`](../docs/exec-plans/tech-debt-tracker.md).
