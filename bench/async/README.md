# `bench/async/` — nanobench scenarios for `oran-async`

## What this bucket benchmarks

`oran-async` owns the executor and backpressure primitives that every higher layer
will sit on. The first scenario measures the cost of the bounded channel handoff
against a direct coroutine-local loop so future mailbox/orchestration work has a
baseline shape.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/channel_ping_pong.cpp`](scenarios/channel_ping_pong.cpp) | Direct coroutine post loop *vs.* `Channel<int>` ping-pong with capacity 1. |

## Running

```sh
xmake build bench-async
xmake run bench-async
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.
