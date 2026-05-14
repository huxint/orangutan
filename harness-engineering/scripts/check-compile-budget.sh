#!/usr/bin/env bash
# Stub for the compile-budget check. Real implementation lands alongside the
# first xmake build skeleton (`docs/exec-plans/active/<date>-mvp-react-loop.md`).
#
# Once implemented:
#   - parses `xmake -v` output or `-ftime-report` for per-TU times
#   - loads `compile_budget.json`
#   - compares against median/p95/hard cap
#   - exits non-zero on hard-cap violation
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v xmake >/dev/null 2>&1; then
  echo "(stub) xmake not available; once provisioned this script will measure per-TU compile times" >&2
  exit 0
fi

if [[ ! -f "${repo_root}/compile_budget.json" ]]; then
  echo "(stub) compile_budget.json not present yet; create when the first xmake build skeleton lands" >&2
  exit 0
fi

echo "(stub) compile-budget check passed (real implementation TBD)"
