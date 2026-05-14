#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

required_files=(
  ".gitignore"
  ".editorconfig"
  ".gitattributes"
  "CODEOWNERS"
  "CONTRIBUTING.md"
  "SECURITY.md"
  ".markdownlint.json"
)

# Once .github/ workflows + PR template land, append:
#   .github/PULL_REQUEST_TEMPLATE.md
#   .github/workflows/ci.yml

failed=0

for path in "${required_files[@]}"; do
  if [[ ! -f "${repo_root}/${path}" ]]; then
    echo "missing required file: ${path}"
    failed=1
  fi
done

if grep -q $'\r' "${repo_root}/README.md"; then
  echo "README.md contains CRLF line endings"
  failed=1
fi

if ! grep -q "make ci" "${repo_root}/CONTRIBUTING.md"; then
  echo "CONTRIBUTING.md should mention make ci"
  failed=1
fi

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "repo hygiene check passed"
