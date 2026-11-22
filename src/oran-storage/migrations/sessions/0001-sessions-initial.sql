CREATE TABLE IF NOT EXISTS sessions(
  session_id TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  title TEXT,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  PRIMARY KEY(session_id, agent_key)
);

CREATE TABLE IF NOT EXISTS session_messages(
  session_id TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  sequence INTEGER NOT NULL,
  role TEXT NOT NULL,
  content_json TEXT NOT NULL,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  PRIMARY KEY(session_id, agent_key, sequence)
);

CREATE INDEX IF NOT EXISTS idx_sessions_agent_updated
  ON sessions(agent_key, updated_at DESC, session_id ASC);

CREATE TRIGGER IF NOT EXISTS trg_session_messages_touch_session
AFTER INSERT ON session_messages
BEGIN
  INSERT INTO sessions(session_id, agent_key, created_at, updated_at)
  VALUES (NEW.session_id, NEW.agent_key, NEW.created_at, NEW.created_at)
  ON CONFLICT(session_id, agent_key) DO UPDATE SET updated_at = NEW.created_at;
END;
