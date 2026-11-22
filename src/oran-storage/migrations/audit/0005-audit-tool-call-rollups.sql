-- audit.db tool-call rollup view.
--
-- Spec 0018 v1.1 needs per-turn, per-tool aggregates over the joined
-- `audit_events` cause chain without adding new columns. This view keeps the
-- source rows authoritative: it groups permission-decision rows by
-- `parent_turn_id` and `tool_name`, exposes hook-publish count from the same
-- turn/tool group, and derives optional latency samples from
-- `metadata_json.usage.wall_time_ms` when the metadata is valid JSON.

CREATE VIEW IF NOT EXISTS audit_tool_call_rollups AS
WITH normalized AS (
  SELECT
    id,
    parent_turn_id,
    tool_name,
    event_kind,
    outcome,
    CASE
      WHEN json_valid(metadata_json) THEN
        CASE
          WHEN json_type(metadata_json, '$.usage.wall_time_ms') IN ('integer', 'real') THEN
            CAST(json_extract(metadata_json, '$.usage.wall_time_ms') AS REAL)
          ELSE NULL
        END
      ELSE NULL
    END AS wall_time_ms
  FROM audit_events
  WHERE parent_turn_id IS NOT NULL
    AND event_kind IN ('permission_decision', 'hook_publish')
)
SELECT
  parent_turn_id,
  tool_name,
  MIN(id) AS first_audit_event_id,
  MAX(id) AS last_audit_event_id,
  SUM(CASE WHEN event_kind = 'permission_decision' THEN 1 ELSE 0 END) AS decision_count,
  SUM(CASE WHEN event_kind = 'hook_publish' THEN 1 ELSE 0 END) AS hook_publish_count,
  SUM(
    CASE
      WHEN event_kind = 'permission_decision'
       AND outcome IN ('allow', 'approved', 'rewritten') THEN 1
      ELSE 0
    END
  ) AS permitted_count,
  SUM(
    CASE
      WHEN event_kind = 'permission_decision'
       AND outcome IN ('deny', 'ask', 'rejected', 'blocked_by_hook') THEN 1
      ELSE 0
    END
  ) AS blocked_count,
  SUM(
    CASE
      WHEN event_kind = 'permission_decision' AND wall_time_ms IS NOT NULL THEN 1
      ELSE 0
    END
  ) AS latency_sample_count,
  COALESCE(
    SUM(
      CASE
        WHEN event_kind = 'permission_decision' THEN wall_time_ms
        ELSE NULL
      END
    ),
    0.0
  ) AS total_wall_time_ms,
  AVG(
    CASE
      WHEN event_kind = 'permission_decision' THEN wall_time_ms
      ELSE NULL
    END
  ) AS average_wall_time_ms
FROM normalized
GROUP BY parent_turn_id, tool_name
HAVING decision_count > 0;
