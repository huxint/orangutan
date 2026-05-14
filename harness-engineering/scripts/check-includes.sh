#!/usr/bin/env bash
# Stub for the include-hygiene check. Once implemented, walks
# include/oran/<lib>/*.hpp and rejects any #include outside the PCH whitelist.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -d "${repo_root}/include/oran" ]]; then
  echo "(stub) include/oran/ not present yet; check is a no-op until C++ skeleton lands"
  exit 0
fi

forbidden=(
  "<nlohmann/json.hpp>"
  "<asio.hpp>"
  "<spdlog/spdlog.h>"
  "<httplib.h>"
  "<sqlite3.h>"
  "<curl/curl.h>"
  "<re2/re2.h>"
  "<thread>"
  "<future>"
  "<stdexec/"
)

failed=0
while IFS= read -r file; do
  for f in "${forbidden[@]}"; do
    if grep -nE "^[[:space:]]*#include[[:space:]]+${f//\//\\/}" "${file}" >/dev/null 2>&1; then
      grep -nE "^[[:space:]]*#include[[:space:]]+${f//\//\\/}" "${file}" \
        | sed "s|^|${file}: forbidden include in public header: |"
      failed=1
    fi
  done
done < <(find "${repo_root}/include/oran" -type f \( -name '*.hpp' -o -name '*.h' \) 2>/dev/null)

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "include-hygiene check passed"
