# `bench-http`

HTTP benchmarks measure client construction and request validation through the
blocking-executor bridge. With the [build prerequisites](../../docs/BUILD_SYSTEM.md)
installed, run from the repository root:

```sh
xmake build -j4 bench-http
xmake run bench-http
```

| Scenario | What it compares |
| --- | --- |
| [`scenarios/client.cpp`](scenarios/client.cpp) | `http.validate_body_request` rejects a TRACE request through the async boundary; `http.construct_client` measures construction of the client and its executor binding. |

The scenario performs no network transfer. Controlled HTTP/SSE tests cover wire
behavior, limits and cancellation.
