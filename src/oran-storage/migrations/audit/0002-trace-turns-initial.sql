-- audit.db trace-turns schema.
--
-- Holds one redacted row per agent turn in the same audit database as
-- `audit_events`. The row is intentionally joinable but body-free: raw prompt
-- bytes, tool inputs, memory facts, and provider responses are not stored here.
-- See docs/product-specs/0018-first-loop-observability.md for the product
-- contract.

CREATE TABLE IF NOT EXISTS trace_turns(
  turn_id BLOB PRIMARY KEY CHECK(length(turn_id) = 16),
  parent_turn_id BLOB CHECK(parent_turn_id IS NULL OR length(parent_turn_id) = 16),
  session_id BLOB NOT NULL CHECK(length(session_id) = 16),
  agent_key TEXT NOT NULL,
  origin TEXT NOT NULL,
  route_profile TEXT NOT NULL,
  route_model TEXT NOT NULL,
  started_at_ns INTEGER NOT NULL,
  finished_at_ns INTEGER NOT NULL,
  stop_reason TEXT NOT NULL,
  iteration_count INTEGER NOT NULL,
  prompt_prefix_hash INTEGER NOT NULL,
  prompt_prefix_bytes INTEGER NOT NULL,
  active_catalog_hash INTEGER NOT NULL,
  deferred_catalog_hash INTEGER NOT NULL,
  cache_creation_tokens INTEGER NOT NULL DEFAULT 0,
  cache_read_tokens INTEGER NOT NULL DEFAULT 0,
  input_tokens INTEGER NOT NULL DEFAULT 0,
  output_tokens INTEGER NOT NULL DEFAULT 0,
  cost_estimate_usd REAL NOT NULL DEFAULT 0,
  cancellation_phase TEXT,
  context_json BLOB NOT NULL DEFAULT X'7b7d',
  schema_version INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS idx_trace_session
  ON trace_turns(session_id, started_at_ns);

CREATE INDEX IF NOT EXISTS idx_trace_agent
  ON trace_turns(agent_key, started_at_ns);
