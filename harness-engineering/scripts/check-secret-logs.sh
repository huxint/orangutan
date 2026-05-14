#!/usr/bin/env bash
# Stub for the secret-log check. Once implemented, greps for logger calls with
# known secret-field names in their arguments.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Heuristic patterns; expand as the codebase grows.
patterns=(
  "log::.*api_key"
  "log::.*client_secret"
  "log::.*password"
  "log::.*ORAN_SECRET_PASSWORD"
)

failed=0
for pat in "${patterns[@]}"; do
  if grep -rnE "${pat}" "${repo_root}/src" "${repo_root}/include" 2>/dev/null; then
    echo "potential secret in log call (pattern: ${pat})"
    failed=1
  fi
done

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "secret-log check passed"
