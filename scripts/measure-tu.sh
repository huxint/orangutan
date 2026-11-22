#!/usr/bin/env bash
# scripts/measure-tu.sh
#
# Per-translation-unit compile-time measurement. Iterates `src/oran-*/*.cpp`
# entries in `compile_commands.json`, re-runs each recorded compile command
# in isolation, and reports wall-clock seconds. Output feeds
# `scripts/check-compile-budget.sh` for the compile-cost investigations in
# `docs/rules/compile-budget.md`.
#
# Usage:
#   scripts/measure-tu.sh [--json]
#
# Pre-conditions:
#   1. `xmake f -m release` has set the configuration that
#      `compile_commands.json` mirrors. Debug flags produce very different
#      timings and would be measuring the wrong thing.
#   2. `xmake build` has run at least once, so every library's PCH
#      (`.../include/oran/cxx/_pch.hpp.gch`) exists. Recorded compile
#      commands reference the PCH via `-include _pch.hpp` and fail without
#      it.
#
# Method:
#   For each TU we delete its .o, re-run the recorded compile, and time the
#   wall clock with bash's `EPOCHREALTIME` (microsecond resolution). A
#   single sample per TU is enough to catch hard-cap regressions;
#   median/p95 budgets in `compile_budget.json` are the noise floor.
#
# Output:
#   Default — human-readable table, slowest TU first.
#   --json  — JSON array on stdout:
#       [
#         { "library": "oran-core", "file": "src/oran-core/error.cpp",
#           "seconds": 0.71 },
#         ...
#       ]
#
# Limitations:
#   - RSS per TU is NOT measured. GNU `time` (the binary) is not present on
#     every CI image. The memory-budget table in `compile-budget.md` is a
#     separate concern; revisit once a TU lands that pressures it.
#   - The per-TU budget applies to library TUs; test and benchmark runners
#     are excluded.
#
# Exit codes:
#   0 — every measured TU compiled successfully.
#   1 — at least one TU failed to compile (error printed to stderr); no
#       JSON / table is emitted.
#   2 — environment problem (missing xmake, jq, compile_commands.json, or
#       a -o argument).

set -euo pipefail

emit_json=0
case "${1:-}" in
--json) emit_json=1 ;;
"") ;;
*)
  echo "measure-tu: unknown argument '${1}' (use --json or no args)" >&2
  exit 2
  ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

cc_json="${repo_root}/compile_commands.json"

if ! command -v xmake >/dev/null 2>&1; then
  echo "measure-tu: xmake not installed; see docs/BUILD_SYSTEM.md" >&2
  exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "measure-tu: jq not installed (required to parse compile_commands.json)" >&2
  exit 2
fi
if [[ ! -f "${cc_json}" ]]; then
  echo "measure-tu: ${cc_json} not found — run 'xmake f -m release' and 'xmake build' first" >&2
  exit 2
fi

# Library TUs only: drop tests/ and bench/. Emit TSV
# `<library>\t<file>` sorted by library then file so the output is stable
# across runs.
mapfile -t lib_files < <(
  jq -r '
    .[]
    | select(.file | test("^src/oran-[a-z][a-z-]*/"))
    | "\(.file | capture("^src/(?<lib>oran-[a-z][a-z-]*)/") | .lib)\t\(.file)"
  ' "${cc_json}" | sort -u
)

if ((${#lib_files[@]} == 0)); then
  echo "measure-tu: no library TUs found in compile_commands.json" >&2
  exit 2
fi

# Buffer results in a temp file; emit on stdout only after every TU
# succeeded so partial output cannot confuse downstream consumers.
results_tsv="$(mktemp)"
trap 'rm -f "${results_tsv}"' EXIT

total="${#lib_files[@]}"
idx=0
for entry in "${lib_files[@]}"; do
  idx=$((idx + 1))
  lib="${entry%%	*}"
  file="${entry#*	}"

  # Extract the recorded argv (one element per line via `-r`).
  mapfile -t args < <(jq -r --arg f "${file}" '
    .[] | select(.file == $f) | .arguments[]
  ' "${cc_json}")
  if ((${#args[@]} == 0)); then
    echo "measure-tu: no arguments for ${file} in compile_commands.json" >&2
    exit 2
  fi

  # Find -o <path> so we can delete the cached object before timing.
  out_path=""
  for ((i = 0; i < ${#args[@]} - 1; i++)); do
    if [[ "${args[i]}" == "-o" ]]; then
      out_path="${args[i + 1]}"
      break
    fi
  done
  if [[ -z "${out_path}" ]]; then
    echo "measure-tu: no -o argument for ${file}" >&2
    exit 2
  fi

  rm -f "${out_path}"

  printf >&2 'measure-tu: [%d/%d] %s ' "${idx}" "${total}" "${file}"

  err_log="$(mktemp)"
  start="${EPOCHREALTIME}"
  if ! "${args[@]}" >/dev/null 2>"${err_log}"; then
    echo >&2
    echo "measure-tu: compile FAILED for ${file}:" >&2
    cat "${err_log}" >&2
    rm -f "${err_log}"
    exit 1
  fi
  end="${EPOCHREALTIME}"
  rm -f "${err_log}"

  # `EPOCHREALTIME` is `<seconds>.<microseconds>`; awk handles the
  # subtraction (bash arithmetic truncates the fractional part).
  elapsed="$(awk -v s="${start}" -v e="${end}" 'BEGIN { printf "%.3f", e - s }')"
  printf >&2 '%.3fs\n' "${elapsed}"
  printf '%s\t%s\t%s\n' "${lib}" "${file}" "${elapsed}" >>"${results_tsv}"
done

if ((emit_json)); then
  jq -Rsn '
    [inputs
     | split("\n")
     | map(select(length > 0))
     | .[]
     | split("\t")
     | { library: .[0], file: .[1], seconds: (.[2] | tonumber) }
    ]
  ' <"${results_tsv}"
else
  printf '%-22s %8s  %s\n' "LIBRARY" "SECONDS" "FILE"
  sort -k3,3 -rn "${results_tsv}" | while IFS=$'\t' read -r lib file secs; do
    printf '%-22s %8.3f  %s\n' "${lib}" "${secs}" "${file}"
  done
fi
