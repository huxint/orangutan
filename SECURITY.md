# Security Policy

## Reporting A Vulnerability

Do **not** open a public issue for a suspected vulnerability.

Instead, report it through a private channel:

- Email the maintainers (replace with project email when the project goes real).
- Include:
  - Affected area and impact.
  - Reproduction steps or proof of concept.
  - Known mitigations or workarounds.

We will acknowledge within 2 business days and aim to provide an initial
assessment within 7 days.

## Scope

Orangutan is a local-first Personal Agent runtime and desktop workbench. Its security boundaries are documented in:

- [Permissions and Hooks](docs/design-docs/permissions-and-hooks.md) — capability evaluation, rule precedence, and audit hooks.
- [Tool Runtime](docs/design-docs/tool-runtime.md) — workspace path containment, symlink policy, and bounded subprocess execution.
- [Secrets and State](docs/design-docs/secrets-and-state.md) — credential reference handling (`api_key_env`), loopback-only local HTTP serving, and private SQLite state.

Sensitive areas include workspace boundary escapes, unauthorized subprocess or file writes, credential leakage in logs/traces/errors, and local HTTP bridge access control.
