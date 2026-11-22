#!/usr/bin/env bash
# scripts/check-prompt-preamble.sh
#
# Keeps the first loop-owned section-1 prompt renderer free of dynamic inputs and
# cross-section prompt bytes.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preamble_file="${repo_root}/src/oran-agent/system_preamble.cpp"
failed=0

fail() {
  echo "[prompt-preamble][FAIL] $*"
  failed=1
}

if [[ ! -f "${preamble_file}" ]]; then
  fail "missing ${preamble_file#${repo_root}/}; section-1 prompt owner must live in oran-agent."
else
  if grep -nE 'now_utc|steady_clock|system_clock|format_iso8601|TurnId|turn_id|request_id|trace_id|session_id|random|uuid' "${preamble_file}"; then
    fail "system preamble renderer contains clocks, ids, or randomness."
  fi

  if grep -nE 'Tool:|tool_catalog|deferred_tools|skills_catalog|memory_framing|conversation_tail|current user|last assistant' "${preamble_file}"; then
    fail "system preamble renderer contains bytes owned by prompt sections 2-5 or 7."
  fi
fi

if [[ "${failed}" -ne 0 ]]; then
  echo "prompt-preamble check failed — keep section 1 stable; see docs/rules/prompt-design.md."
  exit 1
fi

echo "prompt-preamble check passed"
