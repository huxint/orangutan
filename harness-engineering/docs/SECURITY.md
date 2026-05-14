# Security

Use this document to keep secure defaults legible to agents and operators.

## Authentication And Authorization

- The web UI is gated by a single config-defined token (`web.token`).
- Channel adapters carry their own auth model — documented per adapter under
  `docs/design-docs/channel-<name>.md` when noteworthy.
- Multi-user runtimes are a v2 concern; v1 assumes a single trust principal.

## Secret Handling

- Secrets are encrypted at rest via `oran-config` (libsodium `crypto_secretbox`).
  Argon2id KDF; per-field random nonces.
- Plaintext secrets live in memory only behind a `SecretField` accessor; zeroized
  on `Config::~Config`.
- Secrets are never logged. The log shim's redaction filter applies known secret
  field names and runtime regex patterns from config.
- `${VAR}` substitution at config load preserves the "marked secret" flag through
  the substitution.
- Rotation: `orangutan secrets rotate` re-encrypts every marked field under a new
  password.

## Permissions

- Every effectful action passes through `oran-permission`. Bypassing is a critical
  rule violation (see [`rules/critical-rules.md#C10`](rules/critical-rules.md)).
- Default modes documented in [`design-docs/permissions-and-hooks.md`](design-docs/permissions-and-hooks.md).
- Approval prompts are HMAC-signed; the signing key is process-local and discarded
  on shutdown.

## Sandbox Posture

- `shell.exec` runs subprocesses with the runtime's UID. We do not run as root and
  refuse to start if running as root unless `--allow-root` is set.
- Workspace-scoped file operations: `oran-tool-file` constrains writes to the
  workspace root unless explicitly overridden.
- Hardening flags compiled in by default:
  - `_FORTIFY_SOURCE=3`
  - `-fstack-protector-strong`
  - `-fcf-protection`
  - `-fstack-clash-protection`
- ASLR / NX rely on system defaults; CI verifies a release binary has
  `RELRO=full,Now,BindNow`.

## Data Handling

- Session histories may contain PII; the workspace is the responsibility of the
  operator. Document data classification in your deployment's runbook.
- Logs go to `<workspace>/.orangutan/logs/`; rolling daily, 7-day retention by
  default. Operator can extend.
- `audit.db` is append-only (no `DELETE`); retention is a separate operator concern.

## Webhooks And External APIs

- Webhook inbound URLs are HMAC-signed if the caller provides a secret in config.
- Outbound HTTP requests use OpenSSL TLS (via libcurl). Cert pinning is configurable
  via `config.http.pinned_certs`.
- We do not currently support outbound proxy configuration; planned for v1.1.

## Supply Chain

See [`SUPPLY_CHAIN_SECURITY.md`](SUPPLY_CHAIN_SECURITY.md).

## Reporting Vulnerabilities

See repository-root [`../SECURITY.md`](../SECURITY.md).
