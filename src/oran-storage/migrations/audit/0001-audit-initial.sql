-- audit.db initial schema.
--
-- Holds one row per permission decision recorded by the runtime. The
-- columns mirror `permission::AuditEvent` (`include/oran/permission/audit.hpp`)
-- so the upstream sink can write rows without translation. See
-- `docs/design-docs/permissions-and-hooks.md` "Audit" and
-- `docs/product-specs/0008-permissions.md` criterion 1 for the contract.
--
-- The schema is intentionally additive:
--   * Outcomes the agent loop already understands (`allow`/`deny`/`ask`/
--     `approved`/`rejected`) live in `outcome`. `verdict` records the raw
--     rule-engine verdict that produced the outcome (e.g. an `ask` that
--     was later `approved`).
--   * `input_hash_hex` is optional. Allow / deny callsites do not have
--     to compute SHA-256 of the input; approval-flow callsites already
--     have the hash because the `ApprovalAuthority` computed it.
--   * `metadata_json` is the extension point. The current sink writes
--     `{}`; future fields (rule index, channel, hook source) land there
--     before promoting to a real column.

CREATE TABLE IF NOT EXISTS audit_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  scope_key TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  identity TEXT NOT NULL,
  verdict TEXT NOT NULL,
  outcome TEXT NOT NULL,
  reason TEXT NOT NULL,
  input_hash_hex TEXT,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_audit_events_scope_created
  ON audit_events(scope_key, created_at DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_agent_created
  ON audit_events(agent_key, created_at DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_audit_events_outcome
  ON audit_events(outcome, created_at DESC);
