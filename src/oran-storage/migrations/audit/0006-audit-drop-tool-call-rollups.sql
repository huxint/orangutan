-- audit.db retires derived hook-publish reporting.
--
-- Dispatch records one permission-decision row per tool call and no longer
-- writes `hook_publish` rows or post-result usage metadata. The rollup view
-- and the event-kind join index only served those rows. Stored audit rows
-- are untouched; `parent_turn_id` keeps its own index.

DROP VIEW IF EXISTS audit_tool_call_rollups;

DROP INDEX IF EXISTS idx_audit_events_kind_parent_turn;
