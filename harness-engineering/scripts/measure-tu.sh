#!/usr/bin/env bash
# Stub for per-TU compile-time measurement. Real implementation runs
# `xmake -j1 -v` (or `--diagnosis`) and parses cc1plus times.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v xmake >/dev/null 2>&1; then
  echo "(stub) xmake not available; once provisioned this measures per-TU build time" >&2
  exit 0
fi

echo "(stub) measure-tu would emit JSON to stdout (real implementation TBD)"
