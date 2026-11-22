-- audit.db event-kind discriminator.
--
-- Permission decisions were the first `audit_events` row kind. Spec 0018
-- adds joinable hook-publish rows under the same parent-turn cause chain, so
-- the table needs an explicit row-kind discriminator without changing the
-- existing permission-decision payload bytes.

ALTER TABLE audit_events
  ADD COLUMN event_kind TEXT NOT NULL DEFAULT 'permission_decision';

CREATE INDEX IF NOT EXISTS idx_audit_events_kind_parent_turn
  ON audit_events(event_kind, parent_turn_id, id ASC)
  WHERE parent_turn_id IS NOT NULL;
