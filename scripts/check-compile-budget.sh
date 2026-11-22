#!/usr/bin/env bash
# scripts/check-compile-budget.sh
#
# Per-TU compile-time regression gate. Drives `scripts/measure-tu.sh --json`
# and joins each TU's wall-clock time against the per-library thresholds in
# `compile_budget.json`. Implements the contract in
# `docs/rules/compile-budget.md` "Enforcement":
#
#   - Within p95            → green (silent).
#   - Above p95, ≤ hard_cap → yellow (warning line, exit 0).
#   - Above hard_cap        → red   (failure line, exit 1).
#
# Usage:
#   scripts/check-compile-budget.sh [--no-strict] [--show-all]
#
# Flags:
#   --no-strict   Demote hard-cap exceedances to warnings (still exit 0).
#                 Useful when calibrating budgets on new hardware.
#   --show-all    Print every TU's status, including greens. Default only
#                 prints warnings + failures + the summary line.
#
# Exit codes:
#   0 — every measured TU was within its hard cap.
#   1 — at least one TU exceeded its library's hard cap (unless --no-strict).
#   2 — environment problem: xmake / jq missing, `compile_budget.json`
#       missing, a library missing from the budget table, or
#       `measure-tu.sh` itself failed.
#
# Wiring into CI:
#   Run explicitly on reference hardware. Hosted functional builds do not
#   establish the compile-budget gate; see `docs/rules/compile-budget.md`.

set -euo pipefail

strict=1
show_all=0
for arg in "$@"; do
  case "${arg}" in
  --no-strict) strict=0 ;;
  --show-all) show_all=1 ;;
  *)
    echo "check-compile-budget: unknown argument '${arg}'" >&2
    exit 2
    ;;
  esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

budget_json="${repo_root}/compile_budget.json"
measure_sh="${repo_root}/scripts/measure-tu.sh"

if ! command -v xmake >/dev/null 2>&1; then
  echo "check-compile-budget: xmake not installed; see docs/BUILD_SYSTEM.md" >&2
  exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "check-compile-budget: jq not installed (required to parse compile_budget.json)" >&2
  exit 2
fi
if [[ ! -f "${budget_json}" ]]; then
  echo "check-compile-budget: ${budget_json} not found" >&2
  exit 2
fi
if [[ ! -x "${measure_sh}" ]]; then
  echo "check-compile-budget: ${measure_sh} not executable" >&2
  exit 2
fi

tu_json="$(mktemp)"
trap 'rm -f "${tu_json}"' EXIT

# `measure-tu.sh` exits 1 on a compile failure and 2 on env problems;
# either is fatal for the budget check and the script's stderr already
# explains what happened.
if ! "${measure_sh}" --json >"${tu_json}"; then
  echo "check-compile-budget: measure-tu.sh failed; see stderr above" >&2
  exit 2
fi

# Join each `{library, file, seconds}` against the budget triple. Emit a
# stable TSV (`<status>\t<library>\t<seconds>\t<hard_cap>\t<p95>\t<file>`)
# so the bash loop below stays linear and easy to read.
#
# Status values:
#   ok    — seconds ≤ p95_sec.
#   warn  — p95_sec < seconds ≤ hard_cap_sec.
#   fail  — seconds > hard_cap_sec.
#   undef — library has no entry in compile_budget.json (treated as env
#           error: every shipped library must be budgeted in the same
#           commit it gains a TU).
join_tsv="$(mktemp)"
trap 'rm -f "${tu_json}" "${join_tsv}"' EXIT

jq -r --slurpfile budget "${budget_json}" '
  $budget[0].categories as $cats
  | .[]
  | . as $tu
  | ($cats[$tu.library] // null) as $b
  | if $b == null then
      [ "undef", $tu.library, ($tu.seconds | tostring), "-", "-", $tu.file ]
    elif $tu.seconds > $b.hard_cap_sec then
      [ "fail", $tu.library, ($tu.seconds | tostring),
        ($b.hard_cap_sec | tostring), ($b.p95_sec | tostring), $tu.file ]
    elif $tu.seconds > $b.p95_sec then
      [ "warn", $tu.library, ($tu.seconds | tostring),
        ($b.hard_cap_sec | tostring), ($b.p95_sec | tostring), $tu.file ]
    else
      [ "ok", $tu.library, ($tu.seconds | tostring),
        ($b.hard_cap_sec | tostring), ($b.p95_sec | tostring), $tu.file ]
    end
  | @tsv
' "${tu_json}" >"${join_tsv}"

total=0
ok_n=0
warn_n=0
fail_n=0
undef_n=0

while IFS=$'\t' read -r status lib secs cap p95 file; do
  total=$((total + 1))
  case "${status}" in
  ok)
    ok_n=$((ok_n + 1))
    ((show_all)) && printf '  ok    %-22s %6.3fs (p95 %.1fs, cap %.1fs) %s\n' \
      "${lib}" "${secs}" "${p95}" "${cap}" "${file}"
    ;;
  warn)
    warn_n=$((warn_n + 1))
    printf 'WARN   %-22s %6.3fs > p95 %.1fs (cap %.1fs)  %s\n' \
      "${lib}" "${secs}" "${p95}" "${cap}" "${file}" >&2
    ;;
  fail)
    fail_n=$((fail_n + 1))
    printf 'FAIL   %-22s %6.3fs > hard_cap %.1fs (p95 %.1fs)  %s\n' \
      "${lib}" "${secs}" "${cap}" "${p95}" "${file}" >&2
    ;;
  undef)
    undef_n=$((undef_n + 1))
    printf 'UNDEF  %-22s %6.3fs (no compile_budget.json entry)  %s\n' \
      "${lib}" "${secs}" "${file}" >&2
    ;;
  esac
done <"${join_tsv}"

printf 'check-compile-budget: %d TUs (%d ok, %d warn, %d fail, %d undef)\n' \
  "${total}" "${ok_n}" "${warn_n}" "${fail_n}" "${undef_n}"

if ((undef_n > 0)); then
  echo "check-compile-budget: at least one library is missing from compile_budget.json categories — add a row in the same commit that shipped the library" >&2
  exit 2
fi
if ((fail_n > 0)); then
  if ((strict)); then
    echo "check-compile-budget: ${fail_n} TU(s) over hard cap — fail (re-run with --no-strict to demote to a warning while calibrating)" >&2
    exit 1
  else
    echo "check-compile-budget: ${fail_n} TU(s) over hard cap — demoted to warning by --no-strict" >&2
  fi
fi
exit 0
