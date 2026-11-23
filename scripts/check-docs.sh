#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

required_files=(
  "AGENTS.md"
  "CLAUDE.md"
  "README.md"
  "CONTRIBUTING.md"
  "SECURITY.md"
  "Makefile"
  "docs/ARCHITECTURE.md"
  "docs/BUILD_SYSTEM.md"
  "docs/STATUS.md"
  "docs/ROADMAP.md"
  "docs/design-docs/index.md"
  "docs/design-docs/agent-platform.md"
  "docs/design-docs/agent-teams.md"
  "docs/design-docs/api-portability.md"
  "docs/design-docs/async-model.md"
  "docs/design-docs/bootstrap-runtime.md"
  "docs/design-docs/desktop-ui.md"
  "docs/design-docs/io-runtime.md"
  "docs/design-docs/memory-system.md"
  "docs/design-docs/messaging-channels.md"
  "docs/design-docs/permissions-and-hooks.md"
  "docs/design-docs/secrets-and-state.md"
  "docs/design-docs/storage-runtime.md"
  "docs/design-docs/tool-runtime.md"
  "docs/rules/README.md"
  "docs/rules/critical-rules.md"
  "docs/rules/code-style.md"
  "docs/rules/docs-in-sync.md"
  "docs/rules/libraries.md"
  "docs/rules/prompt-design.md"
  "docs/rules/testing-and-bench.md"
)

missing=0
for path in "${required_files[@]}"; do
  if [[ ! -f "${repo_root}/${path}" ]]; then
    echo "missing required file: ${path}"
    missing=1
  fi
done

if ! grep -q "docs/" "${repo_root}/AGENTS.md"; then
  echo "AGENTS.md should point to docs/ as the system of record"
  missing=1
fi

if [[ "${missing}" -ne 0 ]]; then
  exit 1
fi

echo "required-docs check passed"
