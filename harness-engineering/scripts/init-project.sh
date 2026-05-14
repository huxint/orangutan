#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <project-name>" >&2
  exit 1
fi

project_name="$1"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

perl -0pi -e "s/orangutan-v2/${project_name}/g" \
  "${repo_root}/README.md" \
  "${repo_root}/AGENTS.md" \
  "${repo_root}/docs/ARCHITECTURE.md"

echo "Initialized harness-engineering template naming for: ${project_name}"
echo "Next steps:"
echo "  - update docs/ARCHITECTURE.md for the real project shape"
echo "  - update docs/product-specs/index.md with the first product"
echo "  - create scripts/ for project-specific checks"
