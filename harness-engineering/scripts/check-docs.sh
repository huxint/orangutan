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
  "docs/REPO_COLLAB_GUIDE.md"
  "docs/ARCHITECTURE.md"
  "docs/BUILD_SYSTEM.md"
  "docs/FAST_COMPILATION.md"
  "docs/CICD.md"
  "docs/FRONTEND.md"
  "docs/HISTORY_GUIDE.md"
  "docs/PLANS_GUIDE.md"
  "docs/PRODUCT_SENSE.md"
  "docs/QUALITY_SCORE.md"
  "docs/RELIABILITY.md"
  "docs/SECURITY.md"
  "docs/SUPPLY_CHAIN_SECURITY.md"
  "docs/design-docs/index.md"
  "docs/design-docs/core-beliefs.md"
  "docs/design-docs/agent-platform.md"
  "docs/design-docs/module-boundaries.md"
  "docs/design-docs/async-model.md"
  "docs/design-docs/channel-abstraction.md"
  "docs/design-docs/tool-runtime.md"
  "docs/design-docs/memory-system.md"
  "docs/design-docs/team-collaboration.md"
  "docs/design-docs/permissions-and-hooks.md"
  "docs/design-docs/api-portability.md"
  "docs/design-docs/secrets-and-state.md"
  "docs/rules/README.md"
  "docs/rules/critical-rules.md"
  "docs/rules/code-style.md"
  "docs/rules/compile-budget.md"
  "docs/rules/docs-in-sync.md"
  "docs/rules/module-and-pch.md"
  "docs/rules/error-handling.md"
  "docs/rules/async-and-concurrency.md"
  "docs/rules/libraries.md"
  "docs/rules/workflow.md"
  "docs/rules/testing-and-bench.md"
  "docs/product-specs/index.md"
  "docs/product-specs/0001-core-react-loop.md"
  "docs/product-specs/0002-tool-registry.md"
  "docs/product-specs/0003-multi-platform-channels.md"
  "docs/product-specs/0004-agent-team.md"
  "docs/product-specs/0005-memory-system.md"
  "docs/product-specs/0006-automation.md"
  "docs/product-specs/0007-web-ui.md"
  "docs/product-specs/0008-permissions.md"
  "docs/product-specs/0009-skills.md"
  "docs/product-specs/0010-benchmark-harness.md"
  "docs/exec-plans/README.md"
  "docs/exec-plans/templates/execution-plan.md"
  "docs/exec-plans/tech-debt-tracker.md"
  "docs/histories/template.md"
  "docs/references/README.md"
  "docs/references/orangutan-legacy-audit.md"
  "docs/references/harness-template-distill.md"
  "docs/references/third-party-libs.md"
  "docs/releases/README.md"
  "docs/releases/feature-release-notes.md"
  "docs/generated/README.md"
)

missing=0

for path in "${required_files[@]}"; do
  if [[ ! -f "${repo_root}/${path}" ]]; then
    echo "missing required file: ${path}"
    missing=1
  fi
done

for dir in docs/exec-plans/active docs/exec-plans/completed docs/histories; do
  if [[ ! -d "${repo_root}/${dir}" ]]; then
    echo "missing required directory: ${dir}"
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

echo "docs scaffold check passed"
