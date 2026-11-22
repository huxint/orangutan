# bench-agent

Agent benchmarks cover scheduling overhead and audit coordination.

| Scenario | Compares |
| --- | --- |
| [scheduler_overhead.cpp](scenarios/scheduler_overhead.cpp) | Direct registry dispatch and a single-call scheduler batch using the same tool. |
| [scheduler_audit_fanout.cpp](scenarios/scheduler_audit_fanout.cpp) | Eight concurrent calls using a null audit sink and a storage sink over an in-memory pool. |

The scheduler fixture measures fixed batch overhead, including bounded admission, ordered
results and timeout coordination. The audit fixture excludes disk fsync latency
so it measures writer coordination. [bench-prompt](../prompt/README.md) owns pure
prompt rendering; it requires no session state.
