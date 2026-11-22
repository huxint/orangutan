# bench-hook

Hook benchmarks measure advisory publication, blocking decisions and payload
redaction. With the [build prerequisites](../../docs/BUILD_SYSTEM.md) installed,
run from the repository root:

```sh
xmake build -j4 bench-hook
xmake run bench-hook
```

[The scenarios](scenarios/bus.cpp) use callback sinks and a real executor:

| Cases | Measures |
| --- | --- |
| Advisory publication with zero, one and three sinks | Subscription lookup and bounded child work. |
| Blocking publication with zero, one and three proceed sinks | Ordered decision evaluation. |
| Blocking publication with a veto from the second sink | Stopping before later sinks. |
| Large redacted payload with one and three default sinks | Shared immutable payload delivery and redaction cost. |

Compare fan-out cases under the same compiler and host conditions. These runs
exercise dispatch overhead; sink-specific external work is outside the scenarios.
