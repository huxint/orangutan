#!/usr/bin/env bash
# scripts/check-docs-sync.sh
#
# Enforces docs-in-sync.md ("The Prime Directive"): code, build, config, and
# scripts must not drift away from the documentation that describes them.
#
# Some checks are *active today*. The rest are stubs that activate when the
# corresponding artifact lands (xmake build skeleton, C++ source, config schema).
#
# Exits non-zero on any detected drift.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failed=0

note() { echo "[docs-sync] $*"; }
fail() { echo "[docs-sync][FAIL] $*"; failed=1; }

# Files we scan for references. Limit to documentation surfaces, not source.
declare -a doc_surfaces=(
  "${repo_root}/AGENTS.md"
  "${repo_root}/README.md"
  "${repo_root}/CONTRIBUTING.md"
  "${repo_root}/SECURITY.md"
)
while IFS= read -r f; do doc_surfaces+=("$f"); done < <(find "${repo_root}/docs" -type f -name '*.md')

# -----------------------------------------------------------------------------
# Check 1: rules/README.md table mentions every rule file under docs/rules/
# -----------------------------------------------------------------------------
rules_index="${repo_root}/docs/rules/README.md"
if [[ -f "${rules_index}" ]]; then
  while IFS= read -r rule_path; do
    rule_file="$(basename "${rule_path}")"
    [[ "${rule_file}" == "README.md" ]] && continue
    if ! grep -q "\`${rule_file}\`" "${rules_index}"; then
      fail "docs/rules/README.md does not reference ${rule_file}; add it to the table."
    fi
  done < <(find "${repo_root}/docs/rules" -maxdepth 1 -type f -name '*.md' | sort)
fi

# -----------------------------------------------------------------------------
# Check 2: design-docs/index.md references every doc under docs/design-docs/
# -----------------------------------------------------------------------------
design_index="${repo_root}/docs/design-docs/index.md"
if [[ -f "${design_index}" ]]; then
  while IFS= read -r design_path; do
    design_file="$(basename "${design_path}")"
    [[ "${design_file}" == "index.md" ]] && continue
    if ! grep -q "\`${design_file}\`" "${design_index}"; then
      fail "docs/design-docs/index.md does not reference ${design_file}; add it to the catalogue."
    fi
  done < <(find "${repo_root}/docs/design-docs" -maxdepth 1 -type f -name '*.md' | sort)
fi

# -----------------------------------------------------------------------------
# Check 3: product-specs/index.md references every numbered spec
# -----------------------------------------------------------------------------
spec_index="${repo_root}/docs/product-specs/index.md"
if [[ -f "${spec_index}" ]]; then
  while IFS= read -r spec_path; do
    spec_file="$(basename "${spec_path}")"
    [[ "${spec_file}" == "index.md" ]] && continue
    if ! grep -q "${spec_file%.md}" "${spec_index}"; then
      fail "docs/product-specs/index.md does not reference ${spec_file}; add it to the table."
    fi
  done < <(find "${repo_root}/docs/product-specs" -maxdepth 1 -type f -name '*.md' | sort)
fi

# -----------------------------------------------------------------------------
# Check 4: scripts/ entries referenced as backticked tokens or markdown links
# must exist. (Prose mentions are ignored.)
# Patterns matched:
#   `scripts/foo.sh`
#   (scripts/foo.sh)   — markdown link target
# -----------------------------------------------------------------------------
referenced_scripts=$(
  grep -hoE '`scripts/[a-z][a-z0-9_-]+\.sh`|\(scripts/[a-z][a-z0-9_-]+\.sh\)' \
       "${doc_surfaces[@]}" 2>/dev/null \
    | sed -E 's/^`(.*)`$/\1/; s/^\((.*)\)$/\1/' \
    | sort -u
)
while IFS= read -r script_ref; do
  [[ -z "${script_ref}" ]] && continue
  if [[ ! -f "${repo_root}/${script_ref}" ]]; then
    fail "docs reference missing script: ${script_ref}"
  fi
done <<< "${referenced_scripts}"

# -----------------------------------------------------------------------------
# Check 5: Makefile targets referenced as backticked tokens must exist.
# Patterns matched (strict):
#   `make foo`
#   `make foo SLUG=...`
# Free-form prose like "make sure" or "make it" is ignored.
# -----------------------------------------------------------------------------
makefile="${repo_root}/Makefile"
if [[ -f "${makefile}" ]]; then
  referenced_targets=$(
    grep -hoE '`make [a-z][a-z0-9-]+( [A-Z]+=[^`]*)?`' "${doc_surfaces[@]}" 2>/dev/null \
      | sed -E 's/^`make ([a-z0-9-]+).*`$/\1/' \
      | sort -u
  )
  while IFS= read -r mk_target; do
    [[ -z "${mk_target}" ]] && continue
    if ! grep -qE "^${mk_target}:" "${makefile}"; then
      fail "docs reference \`make ${mk_target}\` but Makefile has no such target."
    fi
  done <<< "${referenced_targets}"
fi

# -----------------------------------------------------------------------------
# Check 6 (activates when xmake/packages.lua exists):
# every package in xmake/packages.lua appears in docs/rules/libraries.md.
# -----------------------------------------------------------------------------
packages_lua="${repo_root}/xmake/packages.lua"
libraries_md="${repo_root}/docs/rules/libraries.md"
if [[ -f "${packages_lua}" && -f "${libraries_md}" ]]; then
  while IFS= read -r pkg_line; do
    pkg=$(printf '%s\n' "${pkg_line}" \
            | sed -nE 's/.*add_requires\("([a-zA-Z0-9_-]+)( +([0-9][0-9a-zA-Z.\-]+))?".*/\1|\3/p')
    [[ -z "${pkg}" ]] && continue
    name="${pkg%|*}"
    version="${pkg#*|}"
    if ! grep -q "\`${name}\`" "${libraries_md}"; then
      fail "xmake/packages.lua adds '${name}' but docs/rules/libraries.md does not list it."
    elif [[ -n "${version}" ]] && ! grep -qE "${name}.*${version}" "${libraries_md}"; then
      fail "xmake/packages.lua pins ${name} ${version}; docs/rules/libraries.md disagrees."
    fi
  done < <(grep -E '^[[:space:]]*add_requires\(' "${packages_lua}")
fi

# -----------------------------------------------------------------------------
# Check 7 (activates when xmake/targets.lua exists):
# every oran-* library declared appears in docs/ARCHITECTURE.md.
# -----------------------------------------------------------------------------
targets_lua="${repo_root}/xmake/targets.lua"
architecture_md="${repo_root}/docs/ARCHITECTURE.md"
if [[ -f "${targets_lua}" && -f "${architecture_md}" ]]; then
  while IFS= read -r lib; do
    if ! grep -qE "\`?${lib}\`?" "${architecture_md}"; then
      fail "xmake/targets.lua defines '${lib}' but docs/ARCHITECTURE.md inventory does not list it."
    fi
  done < <(grep -oE 'oran_lib\("[a-z][a-z0-9-]*"' "${targets_lua}" \
            | sed -E 's/oran_lib\("([a-z0-9-]*)"/oran-\1/' | sort -u)
fi

# -----------------------------------------------------------------------------
# Check 8 (activates when src/oran-*/ trees exist):
# every src/oran-<lib>/ has a matching tests/<lib>/ and bench/<lib>/.
# -----------------------------------------------------------------------------
if [[ -d "${repo_root}/src" ]]; then
  while IFS= read -r lib_dir; do
    base="$(basename "${lib_dir}")"
    base="${base#oran-}"
    if [[ ! -d "${repo_root}/tests/${base}" ]]; then
      fail "src/oran-${base}/ has no matching tests/${base}/ (critical-rules.md#C12)."
    fi
    if [[ ! -d "${repo_root}/bench/${base}" ]]; then
      fail "src/oran-${base}/ has no matching bench/${base}/ (critical-rules.md#C12)."
    fi
  done < <(find "${repo_root}/src" -maxdepth 1 -type d -name 'oran-*' 2>/dev/null | sort)
fi

# -----------------------------------------------------------------------------
# Check 9 (stub; activates when include/oran/ has real headers)
# -----------------------------------------------------------------------------
if [[ -d "${repo_root}/include/oran" ]] \
   && find "${repo_root}/include/oran" -name '*.hpp' -print -quit 2>/dev/null | grep -q .; then
  note "code-doc symbol sync check is a planned enhancement; not yet implemented."
fi

# -----------------------------------------------------------------------------
# Check 10 (stub; activates when config.example.json exists)
# -----------------------------------------------------------------------------
if [[ -f "${repo_root}/config.example.json" ]]; then
  note "config-shape sync check is a planned enhancement; not yet implemented."
fi

# -----------------------------------------------------------------------------
if [[ "${failed}" -ne 0 ]]; then
  echo ""
  echo "docs-sync check failed — see lines above. Fix the drift in this PR."
  echo "See docs/rules/docs-in-sync.md for the full change-type → docs-to-update map."
  exit 1
fi

echo "docs-sync check passed"
