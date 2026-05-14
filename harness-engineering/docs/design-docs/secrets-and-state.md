# Secrets And State

This document covers how Orangutan v2 stores configuration, secrets, sessions, memory,
and audit data. The legacy choices (JSON config + AES-256-GCM via mbedtls + four
ad-hoc SQLite tables) are revisited for compile-time and crypto-hygiene reasons.

## Configuration

### File Layout

The default config path is `<workspace>/.orangutan/config.json` (or `--config <path>`).
It contains:

```jsonc
{
  "runtime":     { /* executor sizing, deadlines, redaction patterns */ },
  "permissions": { /* default, allow, deny, ask */ },
  "profiles":    { /* LLM provider profiles */ },
  "routes":      { /* primary + fallbacks per logical route */ },
  "agents":      { /* per-agent overrides */ },
  "teams":       { /* team definitions */ },
  "channels":    { /* per-adapter config */ },
  "hooks":       { /* sinks + bindings */ },
  "memory":      { /* tier policies */ },
  "automation":  { /* job seeds */ },
  "web":         { /* server config */ },
  "session":     { /* auto-save, persistence */ }
}
```

A sample is checked in at `config.example.json` (TBD with first bootstrap PR).

### Schema Validation

We use **JSON Schema** in `docs/generated/config.schema.json` (generated from C++ types
at build time via a small script). At load time, we validate; missing optional fields
take defaults; unknown fields trigger a warning unless `strict_config=true` is set,
in which case they're an error.

This replaces the legacy "silently ignored unknown fields" behavior, which has been a
recurring source of subtle misconfiguration.

### Environment Substitution

`${VAR}` and `${VAR:-default}` substitutions are supported on string values. Done at
load time. Mismatches are errors unless `:-default` syntax provides a fallback.

## Secrets

### Threat Model

We are not protecting against a fully compromised host. We are protecting against:

- Accidentally committing API keys to git (ban via pre-commit hook).
- Accidentally including secrets in logs / hook payloads / web responses (redaction
  via `oran-log`).
- Sharing a config file with a colleague without revealing the secrets (encrypted at
  rest under a password).

### At-Rest Encryption

Secret fields are stored as `enc::<base64(ciphertext)>` strings in the JSON file. The
ciphertext is produced by `crypto_secretbox_easy` (libsodium):

- Algorithm: XSalsa20-Poly1305 AEAD.
- Key derivation: Argon2id from password (config-supplied via `--secret-password` or
  env `ORAN_SECRET_PASSWORD`), salt stored alongside the ciphertext.
- Nonce: 24 bytes random per field; stored alongside.

**Why libsodium instead of mbedtls.** mbedtls is also used for TLS; coupling the
secrets crypto to the TLS stack made the legacy build pay both costs even when only
one was needed. libsodium is a small static library (~200 KB), single-purpose, and
compiles in well under a second. TLS for `oran-http` is handled by the system curl /
OpenSSL.

### Fields Marked Secret

The schema marks fields with `"secret": true`. The config loader:

- Refuses to log them (the logger shim has a redaction filter).
- Returns them via a dedicated `decrypt(field)` accessor.
- Zeroes plaintext buffers on `Config::~Config`.

Legacy "secret-fields" / "secret-protection" continues conceptually; the library is
just smaller and simpler.

### Rotation

The legacy code had no rotation. v2 adds:

- `Config` supports a `secrets_version` integer. The CLI subcommand
  `orangutan secrets rotate --old-password X --new-password Y` re-encrypts every
  marked field under the new password and bumps the version.
- The rotation flow runs offline; the binary refuses to rotate while a runtime is
  active (uses an advisory file lock under `.orangutan/`).

### Approval Signing

Approval prompts (the `ask` permission flow) are HMAC-signed with a key derived from
the process-startup password. The signing key never persists. If the process restarts,
prior approvals become invalid (replay forbidden) — this is correct, not a bug.

## Logging

### Levels

`oran-log` thin shim over spdlog. Levels: `trace, debug, info, warn, error`. Default
console level is `info`; file level is `debug`.

### Redaction

Every log call passes through a redaction filter that:

- Replaces values of known secret fields with `***`.
- Applies config-defined regex patterns (`config.runtime.redaction_patterns`) — runtime
  re2, not compile-time. Examples:
  - `(?i)(api[_-]?key)[=:][^\s]+`
  - `Bearer [A-Za-z0-9_\-\.]+`

The legacy code redacted at the sink boundary, but only for tool output. v2 redacts
in the log shim itself, so all log paths are covered.

### Sinks

- Console sink (color, default).
- Rolling file sink (`<workspace>/.orangutan/logs/orangutan-YYYY-MM-DD.log`, daily,
  7-day retention).
- Optional syslog / journald sink (Linux); opt-in via config.

## Database Files

Four separate SQLite files (one per concern):

- `<workspace>/.orangutan/sessions.db`
- `<workspace>/.orangutan/memory.db`
- `<workspace>/.orangutan/automation.db`
- `<workspace>/.orangutan/audit.db`

### Why Separate Files?

- WAL contention is per-file. Splitting prevents memory-decay's long writer from
  blocking session appends.
- Smaller files copy / replicate / backup faster.
- Each DB has its own migration line so unrelated changes don't conflict.

### Connection Pool

`oran-storage::Pool` (per DB):

- 1 writer connection on an asio strand.
- N reader connections (default 4) drawn round-robin from a pool.
- WAL mode; `synchronous=NORMAL`.
- Prepared-statement cache per connection.

### Expected-Only API

All public APIs return `core::Result<T>` (i.e. `std::expected<T, SqliteError>`). The
legacy throwing wrappers (`must_ok`) **do not exist** in v2. Migration debt is
zero-from-day-one. See `docs/rules/error-handling.md`.

### Migrations

```
src/oran-storage/migrations/
  sessions/
    0001-initial.sql
    0002-add-attachments.sql
  memory/
    0001-initial.sql
    0002-add-team-shared.sql
  ...
```

Each file is a transaction. Applied at startup; recorded in
`schema_versions(version INTEGER PRIMARY KEY, applied_at TEXT)`.

A migration runner verifies monotonic version order; gaps abort startup.

### Backups

`scripts/backup-db.sh` (to be implemented) runs `sqlite3 <file> '.backup ...'` on each
DB. Recommended cron in `docs/CICD.md` once a runtime target exists.

## Identity And Scope

`oran-bootstrap::Identity`:

```cpp
struct Identity {
  std::string agent_key;        // config-defined: "default", "coder", "research"
  std::string runtime_key;      // UUID per process unless pinned by config
  std::string scope_key;        // = runtime_key OR pinned via config
  std::optional<std::string> team_id;
  std::optional<std::string> session_id;
};
```

`scope_key` namespaces:

- Long-term memory records.
- Audit log entries.
- Approval signatures.

Pinning `scope_key` (config: `agent.<name>.scope_pin = true`) causes long-term memory
to persist across runtimes — useful for stable expertise.

## Workspace Layout

```
<workspace>/.orangutan/
├── config.json
├── sessions.db
├── memory.db
├── automation.db
├── audit.db
├── logs/
│   └── orangutan-YYYY-MM-DD.log
├── hooks/                      shell hook scripts
├── skills/                     markdown skills
├── memory/
│   └── MEMORY.md               optional mirror
├── debriefs/                   self-reflective task debriefs (opt-in)
└── lock                        advisory file lock (single live runtime)
```

The presence of `.orangutan/lock` (with PID + start time) tells operators a runtime is
active. Stale locks (PID not running) are cleaned on next startup.

## Anti-Patterns

- Putting secrets in env vars that we then echo elsewhere. Use `${VAR}` substitution
  and let the config layer mark them secret.
- Reading secrets in subprocesses started by tools. The shell-exec tool gets a
  scrubbed environment (no `ORAN_SECRET_PASSWORD` by default).
- Storing derived data in long-term memory. If it's recomputable, leave it out.
- Sharing the same DB file across runtimes without `scope_pin`. The scopes will
  interleave and you'll be confused.

## See Also

- [`memory-system.md`](memory-system.md) — what lives in `memory.db`.
- [`permissions-and-hooks.md`](permissions-and-hooks.md) — what lives in `audit.db`.
- [`../rules/error-handling.md`](../rules/error-handling.md) — `Result<T>` everywhere.
- [`../rules/libraries.md`](../rules/libraries.md) — libsodium / re2 rationales.
