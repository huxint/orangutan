# CI/CD Guide

The repository ships **language-agnostic** CI plumbing on day one. Once C++ code
lands, project-specific verification slots into `scripts/ci.sh` and target-specific
GitHub Actions jobs.

## What Exists By Default

- `.github/workflows/ci.yml` — repository hygiene + docs + shell lint + (once C++
  exists) `xmake build` + `xmake test`.
- `.github/workflows/release.yml` — placeholder release pipeline; replace once a real
  binary exists.
- `.github/workflows/supply-chain-security.yml` — dependency review + OSV scan +
  SBOM generation.

## Design Principle

CI in early scaffolding proves out the delivery plumbing **without pretending to know
the real build command**. As the project grows, the C++ build naturally takes more
of the CI run; the doc/hygiene gates remain.

## Recommended Customization Sequence

1. **Keep** `ci.yml` as the always-on repository gate.
2. **Extend** `scripts/ci.sh` with C++ verification:
   - `xmake f -m release`
   - `xmake -j$(nproc)`
   - `xmake test`
   - `scripts/check-compile-budget.sh`
3. **Replace** `scripts/release-package.sh` with real packaging once binaries exist.
4. **Add** environment-specific deployment jobs once a runtime target exists.
5. **Keep** artifact provenance + SBOM generation in place.

## CI Matrix

Once C++ code lands, the matrix targets:

| Compiler  | Mode    | Modules | LTO  | Sanitizers |
| --------- | ------- | ------- | ---- | ---------- |
| GCC 16.1  | release | yes     | yes  | no         |
| GCC 16.1  | debug   | yes     | no   | asan+ubsan |
| Clang 19  | release | no      | yes  | no         |

Nightly extends with:

- `clang-tidy` full pass.
- Bench suite + comparison.
- Compile-time baseline check.

## Release Workflow Output

The placeholder release pipeline produces:

- `release-manifest.json`
- `repo-metadata.tgz`
- `sbom.spdx.json`
- a GitHub artifact attestation for the packaged artifact

Once binaries exist, this expands to:

- `orangutan-vX.Y.Z-linux-x86_64.tar.gz`
- `orangutan-vX.Y.Z-linux-x86_64.sig`
- `orangutan-vX.Y.Z-debug.tar.gz`
- the same SBOM + attestation.

## Local CI Approximation

```sh
make ci        # docs + hygiene + shell lint
xmake build    # C++ build (once provisioned)
xmake test     # all tests
```

## See Also

- [`SUPPLY_CHAIN_SECURITY.md`](SUPPLY_CHAIN_SECURITY.md)
- [`RELIABILITY.md`](RELIABILITY.md)
- [`BUILD_SYSTEM.md`](BUILD_SYSTEM.md)
- [`rules/workflow.md`](rules/workflow.md)
