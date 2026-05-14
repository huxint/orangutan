# Generated Docs

Reproducible repository knowledge: schema snapshots, API surface indexes, benchmark
baselines, compile-time baselines, dependency inventories.

Generated artifacts:

- Are **reproducible** — checked-in as the canonical version, but a script can
  regenerate them deterministically.
- Are **labeled** with the generator command (in the artifact header).
- Are **dated** in the filename when relevant (`compile-baseline-YYYY-MM-DD.json`).
- Are **diffable** — JSON pretty-printed, deterministic key order.

Examples (some land with the build skeleton, some later):

- `config.schema.json` — JSON Schema for `config.example.json`, generated from
  `oran-config` C++ types.
- `compile-baseline-<date>.json` — output of `scripts/measure-tu.sh`.
- `bench-baseline-<lib>.json` — bench reference numbers per library.
- `pch-spec.json` — canonical PCH contents.
- `web-types.d.ts` — TS types for the web frontend, generated from the C++ route
  handlers.
- `vendored-deps.json` — any vendored-in-source third-party code.

A generated file does **not** belong here if:

- It's regenerated on every build (it should be in `build/`).
- It's only meaningful to one developer (it should be `.gitignore`d).
