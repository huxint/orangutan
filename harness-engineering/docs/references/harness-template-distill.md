# Harness-Template — Distilled Philosophy

This file distills [`iFurySt/harness-template`](https://github.com/iFurySt/harness-template),
the agent-first base template we model on. Read it once; reference the principles when
expanding the framework.

## The Idea

A repository where the **agent's job is straightforward** because the **environment
is set up for it**:

- A short, stable `AGENTS.md` routes to documents under `docs/`.
- `docs/` is the *system of record* — every load-bearing fact lives in a file.
- Execution plans persist intent across sessions.
- Histories capture finished work for the next agent.
- Mechanical checks (`scripts/check-*.sh`) enforce rules without human oversight.

## The Six Working Rules

The harness-template's `core-beliefs.md` distills to:

1. Humans steer; agents execute.
2. Repository-local knowledge beats private human context.
3. The right fix for repeated agent failure is usually better scaffolding, not more
   prompt pressure.
4. Short stable entry points are better than large unstable instruction dumps.
5. Mechanical checks are preferred over soft conventions whenever feasible.
6. Throughput matters, but unmanaged entropy compounds quickly; keep cleanup and
   simplification continuous.

Our [`design-docs/core-beliefs.md`](../design-docs/core-beliefs.md) extends these
with C++23-specific principles.

## The Layout We Adopt

```
AGENTS.md            short routing layer
CLAUDE.md            one-liner: read AGENTS.md
README.md            project intro + quick start
CONTRIBUTING.md      working agreement
SECURITY.md          vulnerability reporting
Makefile             new-plan / new-history / ci
docs/
├── REPO_COLLAB_GUIDE.md      working agreement detail
├── ARCHITECTURE.md           top-level map
├── design-docs/              deep designs (extended for C++23 architecture)
├── product-specs/            features
├── exec-plans/               active + completed + templates
├── histories/                YYYY-MM/YYYYMMDD-HHmm-slug.md
├── references/               curated external + legacy
├── releases/                 user-visible release notes
├── generated/                reproducible artifacts (config schema, benches)
├── PLANS_GUIDE.md            convention
├── HISTORY_GUIDE.md          convention
├── QUALITY_SCORE.md          quality matrix
├── PRODUCT_SENSE.md          product principles
├── RELIABILITY.md            ops
├── SECURITY.md               secure defaults
├── SUPPLY_CHAIN_SECURITY.md  deps / SBOM / provenance
├── CICD.md                   CI/CD scaffolding
└── FRONTEND.md               UI guide
scripts/
├── ci.sh                     base CI
├── check-docs.sh             required-file check
├── check-repo-hygiene.sh     hygiene check
├── new-history.sh
├── new-exec-plan.sh
└── ...                       C++ specific checks (added by us)
.github/
├── PULL_REQUEST_TEMPLATE.md
├── ISSUE_TEMPLATE/
└── workflows/                ci, release, supply-chain-security
```

## What We Extend

The template is generic; we extend it for a C++23 agent runtime:

- `docs/rules/` — non-negotiable code rules (no thread, expected-only, etc.).
  The template uses one `docs/REPO_COLLAB_GUIDE.md` for everything; we promote
  technical rules to their own folder.
- `docs/design-docs/` — deep architectural documents specific to runtime mechanics.
- `docs/BUILD_SYSTEM.md` and `docs/FAST_COMPILATION.md` — the C++ project's biggest
  pain point gets dedicated docs.
- `tests/` and `bench/` directories per library, with `bench/` as a first-class peer.
- C++-specific scripts under `scripts/` (`check-compile-budget.sh`,
  `check-includes.sh`, `measure-tu.sh`, `bench-compare.sh`).

## What We Adopt Verbatim

- The `AGENTS.md` / `CLAUDE.md` / `README.md` / `CONTRIBUTING.md` / `SECURITY.md`
  / `Makefile` shape.
- The `make new-plan` / `make new-history` / `make ci` workflow.
- The exec-plan / history templates.
- The supply-chain workflows + pinning rule.
- The "mechanical checks over soft conventions" enforcement style.

## What We Deliberately Skip From The Original

- The harness-cli `npm` initializer — we do the initialization manually (or via our
  own simpler bootstrap script). The npm dependency wasn't justified for a C++ project.
- Frontend-specific scaffolding in `docs/FRONTEND.md` is kept minimal until the
  web UI shape stabilizes.

## What To Do If The Template Updates

The template is a reference, not a hard dependency. If upstream changes a
convention:

- Compare against our copy in this file.
- Decide whether the change is worth adopting.
- If yes: update the corresponding docs + scripts in one PR.
- If no: note the divergence in `docs/exec-plans/tech-debt-tracker.md` so future
  agents don't accidentally re-introduce it.

## See Also

- Upstream: <https://github.com/iFurySt/harness-template>
- OpenAI's harness engineering write-up (cited by the upstream template):
  <https://openai.com/index/harness-engineering/>
