# `bench/storage/` — nanobench scenarios for `oran-storage`

## What this bucket benchmarks

Storage benchmarks measure SQLite insert paths, compiled and SQL-file migration
startup, statement-cache reuse, pool leases and session/audit/trace persistence.
Install the [build prerequisites](../../docs/BUILD_SYSTEM.md) before running them
from the repository root.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/migrations.cpp`](scenarios/migrations.cpp) | Compiled-span cold/no-op migration runs *vs.* SQL-file load+run cold/no-op migration runs. |
| [`scenarios/sqlite_insert.cpp`](scenarios/sqlite_insert.cpp) | Literal `Connection::execute` inserts *vs.* prepared `Statement` binding. |
| [`scenarios/pool_acquire.cpp`](scenarios/pool_acquire.cpp) | Direct `Connection` re-use *vs.* `Pool::acquire_reader` + `query` for the same SELECT batch, plus uncontended reader acquire batches *vs.* single-slot FIFO waiter-drain contention. |
| [`scenarios/statement_cache.cpp`](scenarios/statement_cache.cpp) | Fresh prepare *vs.* standalone `StatementCache` prepare reuse. |
| [`scenarios/pool_statement_cache.cpp`](scenarios/pool_statement_cache.cpp) | Pool writer fresh prepare *vs.* pool writer slot `StatementCache` reuse. |
| [`scenarios/session_repository.cpp`](scenarios/session_repository.cpp) | Raw pool + cache SQL append/load *vs.* `SessionRepository` append/load. |
| [`scenarios/audit_repository.cpp`](scenarios/audit_repository.cpp) | Raw pool + cache SQL append/count *vs.* `AuditRepository` append/list for a 64-event batch. |
| [`scenarios/trace_repository.cpp`](scenarios/trace_repository.cpp) | Raw pool + cache SQL trace insert *vs.* `TraceRepository` insert for a 32-turn batch. |
| [`scenarios/trace_turn_insert.cpp`](scenarios/trace_turn_insert.cpp) | Raw pool + cache SQL trace insert *vs.* `TraceRepository::append_turn` for a single insert. |

## Running

```sh
xmake build -j4 bench-storage
xmake run bench-storage
```

Output is nanobench's Markdown tables on stdout.
