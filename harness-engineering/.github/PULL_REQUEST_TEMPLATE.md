## Summary

- What changed?
- Why now?
- Cross-link to plan / spec / history if applicable.

## The Prime Directive Check (mandatory)

**Every doc that this PR invalidates is updated in this PR.** No doc-update
follow-ups. See [`docs/rules/docs-in-sync.md`](../docs/rules/docs-in-sync.md).

For each box you check below, confirm the matching docs are updated *in this PR*.
For each unchecked, confirm the area is not touched.

- [ ] No change to public APIs (or matching `docs/design-docs/<area>.md` is updated)
- [ ] No change to config shape / fields (or `config.example.json` + relevant docs updated)
- [ ] No change to CLI flags / subcommands (or `docs/product-specs/<id>.md` + README updated)
- [ ] No change to env vars (or `docs/RELIABILITY.md` "Required environment" updated)
- [ ] No change to xmake targets / packages / options (or `docs/BUILD_SYSTEM.md` + `docs/rules/libraries.md` updated)
- [ ] No change to hook events / capabilities (or `docs/design-docs/permissions-and-hooks.md` + `docs/design-docs/tool-runtime.md` updated)
- [ ] No change to channel / provider / memory / orchestration shape (or matching `docs/design-docs/<area>.md` updated)
- [ ] No change to file layout / conventions (or `docs/ARCHITECTURE.md` + relevant READMEs updated)
- [ ] No change to rules (or `docs/rules/<rule>.md` + `docs/rules/README.md` updated)
- [ ] No change to scripts / Makefile (or referencing READMEs updated)
- [ ] No change to CI workflows (or `docs/CICD.md` + `docs/SUPPLY_CHAIN_SECURITY.md` updated)

## Validation

- [ ] `make ci` green
- [ ] `xmake build` green for affected targets (once C++ skeleton lands)
- [ ] `xmake test` green for affected buckets
- [ ] `scripts/check-compile-budget.sh` green for affected libraries
- [ ] `scripts/check-docs-sync.sh` green (once activated)
- [ ] Relevant tests or manual verification completed
- [ ] **Docs updated for every change that invalidates them** (see Prime Directive above)
- [ ] History entry added (`docs/histories/YYYY-MM/`) — or `History-Skip: <reason>` below
- [ ] Release notes updated if user-visible

## Rule Compliance Self-Check

- [ ] Public headers contain no forbidden includes (see `docs/rules/critical-rules.md#C6`)
- [ ] No new `std::thread` / `std::future` / `stdexec` (see `#C2`)
- [ ] Async functions are cancel-aware (see `#C11`)
- [ ] New tools declare capabilities + go through permission engine (see `#C10`)
- [ ] New third-party packages are documented in `docs/rules/libraries.md` (see `#C15`)
- [ ] Docs match the code being shipped (see `#C16` — Prime Directive)

## Context

- Execution plan:
- Product spec:
- Design doc(s):
- History entry:
- Follow-up debt:

<!--
History-Skip: <one-line reason if intentionally skipping the history entry>
-->
