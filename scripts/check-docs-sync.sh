#!/usr/bin/env bash
# scripts/check-docs-sync.sh
#
# Enforces docs-in-sync.md ("The Prime Directive"): code, build, config, and
# scripts must not drift away from the documentation that describes them.
#
# Active checks: doc-surface indexing (1-5), script/Make/library references
# (4-8), test/bench parity (8), public umbrella header inventory plus
# resolvable header references (9), config.example.json top-level shape (10),
# hook Event catalogue + blocking-set sync (11), and Capability enum + doc
# reference sync (12). Symbol-level public-API extraction remains a planned
# enhancement (check 9's deep half).
#
# Exits non-zero on any detected drift.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failed=0

note() { echo "[docs-sync] $*"; }
fail() {
  echo "[docs-sync][FAIL] $*"
  failed=1
}

# Files we scan for references. Limit to current-contract documentation
# surfaces, not source. Historical archives (completed exec plans, legacy
# references) are excluded — Git is their archive, and they may legitimately
# describe states that no longer exist.
declare -a doc_surfaces=(
  "${repo_root}/AGENTS.md"
  "${repo_root}/README.md"
  "${repo_root}/CONTRIBUTING.md"
  "${repo_root}/SECURITY.md"
)
while IFS= read -r f; do
  case "${f}" in
  */exec-plans/completed/* | */references/*) continue ;;
  esac
  doc_surfaces+=("$f")
done < <(find "${repo_root}/docs" -type f -name '*.md')

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
    "${doc_surfaces[@]}" 2>/dev/null |
    sed -E 's/^`(.*)`$/\1/; s/^\((.*)\)$/\1/' |
    sort -u || true
)
while IFS= read -r script_ref; do
  [[ -z "${script_ref}" ]] && continue
  if [[ ! -f "${repo_root}/${script_ref}" ]]; then
    fail "docs reference missing script: ${script_ref}"
  fi
done <<<"${referenced_scripts}"

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
    grep -hoE '`make [a-z][a-z0-9-]+( [A-Z]+=[^`]*)?`' "${doc_surfaces[@]}" 2>/dev/null |
      sed -E 's/^`make ([a-z0-9-]+).*`$/\1/' |
      sort -u || true
  )
  while IFS= read -r mk_target; do
    [[ -z "${mk_target}" ]] && continue
    if ! grep -qE "^${mk_target}:" "${makefile}"; then
      fail "docs reference \`make ${mk_target}\` but Makefile has no such target."
    fi
  done <<<"${referenced_targets}"
fi

# -----------------------------------------------------------------------------
# Check 6 (activates when xmake/packages.lua exists):
# every package in xmake/packages.lua appears in docs/rules/libraries.md.
# -----------------------------------------------------------------------------
packages_lua="${repo_root}/xmake/packages.lua"
libraries_md="${repo_root}/docs/rules/libraries.md"
if [[ -f "${packages_lua}" && -f "${libraries_md}" ]]; then
  while IFS= read -r pkg_line; do
    pkg=$(printf '%s\n' "${pkg_line}" |
      sed -nE 's/.*add_requires\("([a-zA-Z0-9_-]+)( +([0-9][0-9a-zA-Z.\-]+))?".*/\1|\3/p')
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
  done < <(grep -oE 'oran_lib\("[a-z][a-z0-9-]*"' "${targets_lua}" |
    sed -E 's/oran_lib\("([a-z0-9-]*)"/oran-\1/' | sort -u)
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
# Check 9: public umbrella headers are documented; documented headers exist.
# Header-level sync (symbol-level extraction is a planned enhancement).
# -----------------------------------------------------------------------------
if [[ -d "${repo_root}/include/oran" ]]; then
  umbrella_headers=$(find "${repo_root}/include/oran" -maxdepth 1 -name '*.hpp' -printf '%f\n' 2>/dev/null | sort || true)
  while IFS= read -r header; do
    [[ -z "${header}" || "${header}" == "_pch.hpp" ]] && continue
    if ! grep -qE "\`include/oran/${header}\`|<oran/${header}>" "${doc_surfaces[@]}" 2>/dev/null; then
      fail "public umbrella header include/oran/${header} is not documented; add it to docs/ARCHITECTURE.md."
    fi
  done <<<"${umbrella_headers}"

  # Reverse: every backticked header token in docs must resolve to a real file.
  referenced_headers=$(
    grep -hoE '`include/oran/[a-z][a-z0-9_/-]+\.hpp`|<oran/[a-z][a-z0-9_/-]+\.hpp>' \
      "${doc_surfaces[@]}" 2>/dev/null |
      tr -d '`<>' | sort -u || true
  )
  while IFS= read -r header_ref; do
    [[ -z "${header_ref}" ]] && continue
    header_path="${header_ref#include/}" # `include/oran/...` and <oran/...> spellings
    if [[ ! -f "${repo_root}/include/${header_path}" ]]; then
      fail "docs reference header ${header_ref} but it does not exist."
    fi
  done <<<"${referenced_headers}"
fi

# -----------------------------------------------------------------------------
# Check 10: config.example.json top-level keys match the documented config
# shape in secrets-and-state.md ("The config file contains:" jsonc block).
# -----------------------------------------------------------------------------
example_config="${repo_root}/config.example.json"
config_doc="${repo_root}/docs/design-docs/secrets-and-state.md"
if [[ -f "${example_config}" && -f "${config_doc}" ]]; then
  actual_keys=$(grep -oE '^  "[a-z][a-z_]*":' "${example_config}" | tr -d '": ' | sort -u || true)
  documented_keys=$(
    sed -n '/The config file contains:/,/^```$/p' "${config_doc}" |
      grep -oE '^\s*"[a-z][a-z_]*":' | tr -d '": ' | sort -u || true
  )
  while IFS= read -r key; do
    [[ -z "${key}" ]] && continue
    if ! grep -q "^${key}$" <<<"${documented_keys}"; then
      fail "config.example.json has top-level key '${key}' that secrets-and-state.md does not document."
    fi
  done <<<"${actual_keys}"
  while IFS= read -r key; do
    [[ -z "${key}" ]] && continue
    if ! grep -q "^${key}$" <<<"${actual_keys}"; then
      fail "secrets-and-state.md documents config key '${key}' that config.example.json does not contain."
    fi
  done <<<"${documented_keys}"
fi

# -----------------------------------------------------------------------------
# Check 11: hook Event enum matches the permissions-and-hooks.md catalogue and
# the `is_gate` cases match the documented blocking set.
# -----------------------------------------------------------------------------
event_header="${repo_root}/include/oran/hook/event.hpp"
events_doc="${repo_root}/docs/design-docs/permissions-and-hooks.md"
if [[ -f "${event_header}" && -f "${events_doc}" ]]; then
  actual_events=$(sed -n '/enum class Event/,/^};/p' "${event_header}" | grep -oE '[a-z_0-9]+,' | tr -d ' ,' | sort -u || true)
  doc_events=$(sed -n '/enum class Event {/,/^};/p' "${events_doc}" | grep -oE '[a-z_0-9]+,' | tr -d ' ,' | sort -u || true)
  while IFS= read -r event; do
    [[ -z "${event}" ]] && continue
    if ! grep -q "^${event}$" <<<"${doc_events}"; then
      fail "hook Event::${event} is not listed in the permissions-and-hooks.md catalogue."
    fi
  done <<<"${actual_events}"
  while IFS= read -r event; do
    [[ -z "${event}" ]] && continue
    if ! grep -q "^${event}$" <<<"${actual_events}"; then
      fail "permissions-and-hooks.md catalogue lists Event::${event} which does not exist in event.hpp."
    fi
  done <<<"${doc_events}"

  blocking_set=$(sed -n '/is_gate(Event event)/,/^}/p' "${event_header}" | grep -oE 'case Event::[a-z_0-9]+:' | sed 's/case Event:://; s/://' | sort -u || true)
  doc_blocking=$(
    grep -oE '^- \*\*Blocking\*\*.*$' "${events_doc}" |
      grep -oE '`[a-z_0-9]+`' | tr -d '`' | sort -u || true
  )
  while IFS= read -r event; do
    [[ -z "${event}" ]] && continue
    if ! grep -q "^${event}$" <<<"${blocking_set}"; then
      fail "documented blocking event '${event}' is not a case of hook::is_gate in event.hpp."
    fi
  done <<<"${doc_blocking}"
  while IFS= read -r event; do
    [[ -z "${event}" ]] && continue
    if ! grep -q "^${event}$" <<<"${doc_blocking}"; then
      fail "hook::is_gate lists Event::${event}, which is not in the documented blocking set."
    fi
  done <<<"${blocking_set}"
fi

# -----------------------------------------------------------------------------
# Check 12: Capability enum matches the tool-runtime.md capability block, and
# every backticked Capability:: reference in docs resolves to a real
# enumerator.
# -----------------------------------------------------------------------------
capability_header="${repo_root}/include/oran/core/capability.hpp"
capability_doc="${repo_root}/docs/design-docs/tool-runtime.md"
if [[ -f "${capability_header}" && -f "${capability_doc}" ]]; then
  actual_caps=$(sed -n '/enum class Capability/,/^};/p' "${capability_header}" | grep -oE '[a-z_0-9]+,' | tr -d ' ,' | sort -u || true)
  doc_caps=$(sed -n '/enum class Capability {/,/^};/p' "${capability_doc}" | grep -oE '[a-z_0-9]+,' | tr -d ' ,' | sort -u || true)
  while IFS= read -r cap; do
    [[ -z "${cap}" ]] && continue
    if ! grep -q "^${cap}$" <<<"${doc_caps}"; then
      fail "Capability::${cap} is not listed in the tool-runtime.md capability block."
    fi
  done <<<"${actual_caps}"
  while IFS= read -r cap; do
    [[ -z "${cap}" ]] && continue
    if ! grep -q "^${cap}$" <<<"${actual_caps}"; then
      fail "tool-runtime.md capability block lists '${cap}' which does not exist in capability.hpp."
    fi
  done <<<"${doc_caps}"

  referenced_caps=$(grep -hoE '`Capability::[a-z_0-9]+`' "${doc_surfaces[@]}" 2>/dev/null |
    tr -d '`' | sed 's/Capability:://' | sort -u || true)
  while IFS= read -r cap; do
    [[ -z "${cap}" ]] && continue
    if ! grep -q "^${cap}$" <<<"${actual_caps}"; then
      fail "docs reference Capability::${cap} which does not exist in include/oran/core/capability.hpp."
    fi
  done <<<"${referenced_caps}"
fi

# -----------------------------------------------------------------------------
if [[ "${failed}" -ne 0 ]]; then
  echo ""
  echo "docs-sync check failed — see lines above. Fix the drift in this PR."
  echo "See docs/rules/docs-in-sync.md for the full change-type → docs-to-update map."
  exit 1
fi

echo "docs-sync check passed"
