#!/usr/bin/env bash
# Reject GitHub Actions referenced by floating tags (e.g. @v1, @main).
# Workflow actions must be pinned to immutable commit SHAs.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workflows_dir="${repo_root}/.github/workflows"

if [[ ! -d "${workflows_dir}" ]]; then
  echo "no .github/workflows dir; skipping action-pinning check"
  exit 0
fi

failed=0
while IFS= read -r file; do
  while IFS= read -r line; do
    # Match `uses: owner/name@<ref>`; ref must be a 40-char hex SHA.
    if [[ "${line}" =~ uses:[[:space:]]+([^@[:space:]]+)@([^[:space:]]+) ]]; then
      ref="${BASH_REMATCH[2]}"
      if [[ ! "${ref}" =~ ^[a-f0-9]{40}$ ]]; then
        echo "${file}: action '${BASH_REMATCH[1]}' pinned to '${ref}' — must be a 40-char SHA"
        failed=1
      fi
    fi
  done < "${file}"
done < <(find "${workflows_dir}" -type f -name '*.yml' | sort)

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "action-pinning check passed"
