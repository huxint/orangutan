# Change History Guide

Every code-change task that modifies repository behavior produces a **history entry**
under `docs/histories/YYYY-MM/`. Pure research or Q&A tasks do not need one unless they
result in repository changes.

> A history entry is **not** a substitute for keeping the rest of the docs in sync.
> See [`rules/docs-in-sync.md`](rules/docs-in-sync.md) — every PR that changes
> behavior updates **both** the relevant production docs **and** writes a history
> entry. Histories record *why* a change was made; production docs describe *the
> current state of the system*.

## Why

The next agent has no chat memory. The history entry is how it learns *why* the code
looks the way it does. Git log helps, but it captures *what* — the history file
captures *intent*, *alternatives considered*, and *files of interest*.

## Layout

```
docs/histories/
├── template.md
└── 2026-05/
    └── 20260514-1430-mvp-react-loop.md
```

- Directory: `docs/histories/YYYY-MM/` (created on first use of the month).
- Filename: `YYYYMMDD-HHmm-task-slug.md`.
- Use `make new-history SLUG=<slug>` to scaffold.

## What To Include

- The user request, redacted if it contains sensitive data.
- A concise summary of *what changed* (areas, key files, public-surface deltas).
- The *design intent*: why this approach, what alternatives were rejected.
- The execution-plan reference if one existed.
- The release-note reference if user-visible.
- Any tech-debt notes left behind, with links to the tracker entry.

## What Not To Include

- Secrets, API keys, internal user identifiers.
- Verbatim chat logs.
- Speculative future work — link to an exec plan instead.

## Length

A history entry is **a paragraph or two plus a bulleted file list**. Long entries are
usually a sign that the change should have been broken into multiple tasks.

## Updates Within A Task

If a task spans multiple rounds, **update the same history file**. Do not create
duplicates per round.

## Enforcement

`scripts/check-history-touched.sh` (TBD with build skeleton) flags PRs that change
code without touching `docs/histories/`. A PR may override with a trailer:

```
History-Skip: <one-line reason>
```

…in the PR description.

## Template

See [`histories/template.md`](histories/template.md).
