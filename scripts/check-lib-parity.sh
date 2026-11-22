#!/usr/bin/env bash
# Each library in xmake/targets.lua needs tests/<lib>/ and bench/<lib>/ buckets.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f "${repo_root}/xmake/targets.lua" ]]; then
  echo "xmake/targets.lua is missing" >&2
  exit 1
fi

failed=0
while IFS= read -r lib; do
  base="${lib#oran-}"
  if [[ ! -d "${repo_root}/tests/${base}" ]]; then
    echo "tests/${base}/ missing for oran-${base}"
    failed=1
  fi
  if [[ ! -d "${repo_root}/bench/${base}" ]]; then
    echo "bench/${base}/ missing for oran-${base}"
    failed=1
  fi
done < <(grep -oE 'oran_lib\("[a-z][a-z0-9-]*"' "${repo_root}/xmake/targets.lua" \
          | sed -E 's/oran_lib\("([a-z0-9-]*)"/oran-\1/')

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "library-parity check passed"
