CREATE TABLE IF NOT EXISTS session_skill_activations(
  session_id TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  skill_name TEXT NOT NULL,
  active INTEGER NOT NULL CHECK(active IN (0, 1)),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  PRIMARY KEY(session_id, agent_key, skill_name)
);

CREATE INDEX IF NOT EXISTS idx_session_skill_activations_session_active
  ON session_skill_activations(session_id, agent_key, active, skill_name);
