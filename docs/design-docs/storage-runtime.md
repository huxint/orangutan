# Storage

`oran-storage` exposes expected-returning SQLite operations. It owns connections,
statements, migrations, bounded pool leases and session/audit/trace repositories.
Domain libraries own their own schemas above this boundary.

## Ownership And Execution

Connections, statements and transactions are RAII resources. Statements bind
parameters, positionally through `bind_all` for fixed parameter lists; SQL
values never enter queries by string interpolation. A `Transaction` begins
IMMEDIATE and rolls back on destruction unless its commit succeeded. A `Pool` owns one writer
and a finite set of readers. Leases are exclusive and keep shared pool state
alive. Connection setup configures the opened handle from explicit options.
Per-connection statement caches are bounded and returned statements reset before
reuse.

Acquiring a lease is asynchronous and cancellation-aware. Available, queued and
cancelled lease completions resume on the requesting coroutine's executor. SQL
after acquisition runs there too; the pool's executor only drives availability.

Runtime session, audit and trace operations start on the blocking executor and
await completion back on the coordinating strand. `StorageAuditSink` binds its
worker at construction; `agent::TraceContext` supplies one for terminal trace
writes. Missing worker bindings return an explicit error. Requests own their
values across suspension, and repository owners await all writes before releasing
borrowed services. Audit and trace writers report cancellation after joining the
pending operation; an already committed row remains durable.

## Migrations

Versioned migrations run transactionally and report previous/current/applied
versions. Built-in audit and session SQL is embedded from files under
`src/oran-storage/migrations`; installed binaries do not discover source files
from their current directory. Callers may supply an explicit migration directory.
The long-term memory schema belongs to `oran-memory`.

Backups before schema changes and explicit ownership/scope import mappings are
required before a data-format migration is introduced. Runtime reductions may
drop derived views and indexes but leave persisted user rows intact. The explicit
backup and session-import APIs below support this boundary; broader long-term
memory backup and explicit scope imports are owned by the
[memory contract](memory-system.md#long-term-memory-recovery).

The session skill table (`session_skill_activations`) remains accessible through
SQLite for compatibility. Audit migration 6 drops the derived
`audit_tool_call_rollups` view and its event-kind index; stored rows, including
older `hook_publish` rows, are untouched.
Reopening a database and appending runtime records preserves saved session
metadata, skill rows, audit decisions and traces. Migration history is retained.

## Backup And Session Import

`Connection::backup_to` uses SQLite `VACUUM INTO`, capturing committed WAL data,
and verifies the result with `integrity_check`. It requires a new destination;
`SessionRepository::backup_to` acquires a reader and delegates. These are explicit
host maintenance operations on the blocking executor. The host owns private
source/destination directories and must keep their paths stable for the operation.
Take and verify a backup before applying a user-data format migration. A failed
backup may leave an incomplete destination and never reports it as successful.

`SessionRepository::import_session` opens a source snapshot read-only and requires
explicit source/destination session and agent keys. The supported source schema is
the existing session schema (version 2). It copies message sequences, content,
metadata, timestamps and skill rows in one destination transaction, preserving the
session title, metadata and working checkpoint. Any existing destination identity
(including orphan message/skill rows), missing source, SQL failure or observed
cancellation aborts the import. Source rows and migration history are untouched.
No memory scope, approval grant, audit or trace authority is imported. This narrow
API supports session recovery and identity mapping; it is not a general database
merge tool.

## Repositories

- `SessionRepository` keys sessions by session ID and agent key. Appends allocate
  monotonically increasing sequence numbers; reads preserve conversation order.
  `append_messages` accepts one key and an owned sequence of encoded messages.
  It validates the complete input, acquires one writer lease and commits all
  inserts and session-row updates in one transaction. Any later insert or commit
  failure rolls back the suffix; an empty suffix does not create a session.
  Single-message append delegates to this transaction boundary. Optional
  `SessionCommit` compares the loaded message count and checkpoint revision before
  writing, then updates only the reserved working-context metadata key in the same
  transaction. Coverage cannot exceed the committed transcript. Invalid metadata
  and stale snapshots fail without appending any rows. `get_session`
  reads one session's metadata and message count by the same composite key.
- `load_after` reads ascending rows between explicit sequence bounds in bounded
  pages (default 64 rows/512 KiB). It rejects gaps and an oversized first row so a
  caller cannot mistake omitted history for completion.
- `load_tail` reads a contiguous newest suffix bounded by row count and UTF-8
  encoded content/metadata bytes, then returns ascending sequence order. Defaults
  are 128 rows/512 KiB. The API never prunes stored messages.
- `AuditRepository` appends decision rows and lists them by scope, newest first,
  with agent, tool, event-kind and outcome filters. Rows are never updated.
  `StorageAuditSink` adapts this repository to dispatch. Decision writes must
  complete before the handler can run. Storage failure or cancellation cannot
  grant an effect.
- `TraceRepository` records redacted turn metadata: IDs, origin, model/route,
  prompt hashes/byte counts, usage, timing and stop/cancellation classification.
  Records can be read by turn ID or listed with session/agent filters and a
  limit. Raw provider bodies and secret values do not belong in trace rows.

Empty required keys, invalid limits and malformed rows return explicit errors.
Transactions that do not commit roll back. Session serialization finishes before
the writer transaction starts; the repository retains the existing schema and
allocates each suffix's sequence numbers while holding that transaction.
