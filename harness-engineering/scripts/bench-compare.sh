#!/usr/bin/env bash
# Run a library's benchmark, compare to its baseline, fail on > 10% regression.
#
# Usage:
#   scripts/bench-compare.sh <library>
#
# Examples:
#   scripts/bench-compare.sh memory
#   scripts/bench-compare.sh provider
#
# Reads:    docs/generated/bench-baseline-<library>.json
# Writes:   docs/generated/bench-output-<library>-<date>.json
#
# This script is a *spec*: once xmake/build is provisioned and the bench-<library>
# targets are real, it runs them; until then it prints what it would do.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <library>" >&2
  exit 1
fi

lib="$1"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
date_tag="$(date +%Y-%m-%d)"

baseline="${repo_root}/docs/generated/bench-baseline-${lib}.json"
output="${repo_root}/docs/generated/bench-output-${lib}-${date_tag}.json"

if ! command -v xmake >/dev/null 2>&1; then
  echo "(skeleton) xmake not available; would run:" >&2
  echo "  xmake build bench-${lib}" >&2
  echo "  xmake run bench-${lib} --json > ${output}" >&2
  exit 0
fi

if ! xmake build "bench-${lib}" >/dev/null 2>&1; then
  echo "could not build bench-${lib}; is it implemented yet?" >&2
  exit 2
fi

xmake run "bench-${lib}" --json > "${output}"

if [[ ! -f "${baseline}" ]]; then
  echo "no baseline at ${baseline}; saving current run as baseline"
  cp "${output}" "${baseline}"
  exit 0
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required for comparison; install jq" >&2
  exit 3
fi

# Compare median nanoseconds per scenario; fail on > 10% regression.
regress=$(jq -s --arg threshold "1.10" '
  .[0] as $base
  | .[1] as $now
  | $now.results
  | map(. as $cur
        | (($base.results // []) | map(select(.bench == $cur.bench))[0]) as $b
        | select($b != null)
        | {bench: .bench,
           base_median: $b.median_ns,
           now_median:  .median_ns,
           ratio:       (.median_ns / $b.median_ns)}
        | select(.ratio > ($threshold | tonumber)))
' "${baseline}" "${output}")

if [[ -n "${regress}" && "${regress}" != "[]" ]]; then
  echo "Regression detected (>10% slower than baseline):"
  echo "${regress}" | jq -r '.[] | "  - \(.bench): \(.now_median)ns (was \(.base_median)ns, +\((.ratio - 1) * 100 | floor)%)"'
  exit 4
fi

echo "bench-compare(${lib}): no significant regression vs baseline"
