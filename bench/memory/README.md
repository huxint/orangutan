# `bench/memory`

Memory benchmarks measure conversation serialization overhead and scoped lexical
search. With the [build prerequisites](../../docs/BUILD_SYSTEM.md) installed, run
from the repository root:

```sh
xmake build -j4 bench-memory
xmake run bench-memory
```

| Scenario | Compares |
| --- | --- |
| [`scenarios/session_store.cpp`](scenarios/session_store.cpp) | Raw `storage::SessionRepository` append/load of one 64-message batch vs. typed `memory::session::Store` append/load of the same logical conversation. |
| [`scenarios/longterm_fts5.cpp`](scenarios/longterm_fts5.cpp) | `Fts5Backend::search` over a seeded 10k-record corpus at `limit=10`. |

The lexical scenario measures search only; migration and corpus seeding happen
before timing. Each scenario owns temporary database files and removes them when
finished.
