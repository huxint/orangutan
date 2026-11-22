-- audit.db parent-turn correlation.
--
-- Adds the typed join key spec 0018 needs between trace_turns and each
-- permission decision emitted during that turn. The column is nullable so
-- existing audit rows and trace-disabled runtimes keep byte-equivalent
-- decision payloads apart from the additive schema.

ALTER TABLE audit_events ADD COLUMN parent_turn_id BLOB NULL;

CREATE INDEX IF NOT EXISTS idx_audit_events_parent_turn
  ON audit_events(parent_turn_id, id ASC)
  WHERE parent_turn_id IS NOT NULL;
