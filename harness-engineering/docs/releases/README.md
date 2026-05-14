# Release Notes Guide

Use `feature-release-notes.md` for user-visible launches, improvements, and fixes.

## Rules

- Group entries by month with `## YYYY-MM`.
- Insert the newest release at the top of the matching month section.
- Focus on user-visible impact first, implementation summary second.
- Do not clutter this file with purely internal refactors — those go in
  `docs/histories/`.
- Cross-link the matching history entry when appropriate.

## Suggested Columns

- Date
- Area
- User Impact
- Change Summary
- History link

## What Counts As User-Visible

- New CLI flag, new web route, new tool, new channel adapter.
- Changed default behavior (permissions tightened, retry policy adjusted).
- Performance improvement noticeable to operators (≥ 10% reduction in iteration
  overhead, ≥ 30% reduction in build time, etc.).
- Configuration shape changes (new config section, deprecated field).

## What Doesn't

- Internal refactors with no behavior change.
- Doc-only updates (those go in the history entries that drive them).
- Bench / test additions.
