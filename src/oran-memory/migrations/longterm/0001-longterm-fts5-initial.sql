CREATE TABLE IF NOT EXISTS longterm_records(
  scope_key TEXT NOT NULL,
  id TEXT NOT NULL,
  kind TEXT NOT NULL,
  title TEXT NOT NULL,
  body TEXT NOT NULL,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  last_read_at TEXT NOT NULL,
  importance REAL NOT NULL CHECK(importance >= 0.0 AND importance <= 1.0),
  tags_json TEXT NOT NULL DEFAULT '[]',
  linked_record_ids_json TEXT NOT NULL DEFAULT '[]',
  shadow INTEGER NOT NULL DEFAULT 0 CHECK(shadow IN (0, 1)),
  PRIMARY KEY(scope_key, id)
);

CREATE INDEX IF NOT EXISTS idx_longterm_records_scope_kind_shadow_updated
  ON longterm_records(scope_key, kind, shadow, updated_at DESC, id ASC);

CREATE VIRTUAL TABLE IF NOT EXISTS longterm_records_fts USING fts5(
  scope_key UNINDEXED,
  record_id UNINDEXED,
  kind UNINDEXED,
  shadow UNINDEXED,
  title,
  body,
  tags,
  tokenize = 'unicode61'
);
