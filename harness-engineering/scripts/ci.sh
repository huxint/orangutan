#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${repo_root}/scripts/check-docs.sh"
"${repo_root}/scripts/check-repo-hygiene.sh"
"${repo_root}/scripts/check-docs-sync.sh"

if [[ -f "${repo_root}/scripts/check-action-pinning.sh" ]]; then
  "${repo_root}/scripts/check-action-pinning.sh"
fi

# Validate shell scripts parse cleanly.
while IFS= read -r file; do
  bash -n "$file"
done < <(find "${repo_root}/scripts" -type f -name '*.sh' | sort)

# Optional: shellcheck if available
if command -v shellcheck >/dev/null 2>&1; then
  shellcheck "${repo_root}/scripts/"*.sh || true
fi

# Once xmake is provisioned, this script will also run:
#   xmake f -m release
#   xmake -j$(nproc)
#   xmake test
#   scripts/check-compile-budget.sh

echo "base CI checks passed"
