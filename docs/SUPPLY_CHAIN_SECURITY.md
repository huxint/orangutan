# Supply Chain

Third-party versions and boundaries live in [libraries](rules/libraries.md), with
build requirements in `xmake/packages.lua`. Public headers keep JSON, SQLite and
curl implementation types private.

GitHub Actions and the GCC container are pinned in `.github/workflows/ci.yml`.
`scripts/check-action-pinning.sh` validates action pins. System libcurl and its
TLS implementation follow the host's package updates.

Xmake's resolver lock is currently a machine-local artifact. A reproducible
hosted lock refresh/check workflow and analyzer evidence remain tracked quality
work. Repository checks do not substitute for dependency vulnerability review.
