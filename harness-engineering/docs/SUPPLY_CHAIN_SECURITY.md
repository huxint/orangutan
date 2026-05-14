# Supply Chain Security

Defaults inherit from the harness-template template and tighten to fit a C++ project
that ships native binaries.

## Default Controls

- **Dependency pinning**: `xmake-requires.lock` pins every package by sha + version.
- **Dependency review**: enabled via `actions/dependency-review-action` on PRs.
- **Vulnerability scanning**: `google/osv-scanner-action` on PRs, scheduled runs, and
  manual dispatch. Picks up `xmake-requires.lock`.
- **SBOM**: `anchore/sbom-action` produces an SPDX SBOM for release artifacts.
- **Provenance**: `actions/attest-build-provenance` generates a signed attestation
  for each release.
- **Pinned actions**: every workflow pins GitHub Actions to immutable commit SHAs.
- **No floating tags**: `scripts/check-action-pinning.sh` fails CI if it spots
  floating tag references.

## Current Workflow Mapping

| Workflow file                     | Role |
| --------------------------------- | ---- |
| `.github/workflows/ci.yml`        | Docs / hygiene / shell-lint / xmake test |
| `.github/workflows/release.yml`   | Release-package job (placeholder until v1 binaries land) |
| `.github/workflows/supply-chain-security.yml` | Dependency review + OSV scan |

## Limits And Assumptions

- Dependency Review is fully available on public repositories; private repositories
  need GitHub Advanced Security.
- OSV picks up xmake lockfile + system-package manifests; for sources we vendor
  directly (rare), the manifest is encoded in `docs/generated/vendored-deps.json`.
- SBOM quality depends on `xmake/packages.lua` listing canonical versions and
  source URLs.
- Provenance is meaningful once `scripts/release-package.sh` reflects the real
  binary build.
- **OpenSSF Scorecard** intentionally disabled until branch protection, release
  history, and SAST posture exist. Add back once the rules are real.

## C++ Specific Notes

- **Static binaries**: when we ship a fully static binary (musl-clang stretch), the
  SBOM must list every library statically linked.
- **Bundled C dependencies** (libcurl + OpenSSL + sqlite3 + libsodium): we prefer
  system packages on production hosts; lockfile pins the versions used in `xmake`
  source builds for reproducibility.
- **Random hashing libraries**: `rapidhash` is a small inline lib; `simdutf` is a
  larger native code dep — both are listed in the SBOM.

## Reviewer Checklist For New Packages

- Is the package in `docs/rules/libraries.md`?
- Is the version pinned in `xmake-requires.lock`?
- Is the source URL canonical (upstream repo, not a mirror unless required)?
- License compatibility verified?
- Compile-cost estimate recorded?
- For optional packages, gated behind an `xmake/options.lua` flag?

## What To Do When The Project Becomes Real

- Add ecosystem-specific lockfiles and keep them committed.
- Make the release build deterministic and produce explicit versioned artifacts.
- Gate production deployment on provenance verification when possible.
- Consider verifying attestations in the deployment environment (cluster admission,
  endpoint protection, etc.).

## Related Reading

- [`SECURITY.md`](SECURITY.md)
- [`CICD.md`](CICD.md)
- [`rules/libraries.md`](rules/libraries.md)
