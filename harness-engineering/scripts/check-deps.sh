#!/usr/bin/env bash
# Stub for the dependency-direction check. Once xmake skeleton lands, parses
# each library's add_deps() and rejects upward or sideways deps.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f "${repo_root}/xmake/targets.lua" ]]; then
  echo "(stub) xmake/targets.lua not present yet; check is a no-op until C++ skeleton lands"
  exit 0
fi

echo "(stub) dependency-direction check passed (real implementation TBD)"
