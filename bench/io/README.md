# `bench/io/` — nanobench scenarios for `oran-io`

## What this bucket benchmarks

These scenarios measure the descriptor-based file boundary, metadata reads and
bounded line ranges. Each coroutine read opens and reads the file; repeated calls
include that work. Configure the [supported build](../../docs/BUILD_SYSTEM.md)
before running this bucket from the repository root.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/file_read.cpp`](scenarios/file_read.cpp) | Direct `std::ifstream` text read *vs.* `io::read_text_file` through an asio coroutine. |
| [`scenarios/fingerprint.cpp`](scenarios/fingerprint.cpp) | Filesystem size/mtime queries *vs.* `io::compute_file_fingerprint`. |
| [`scenarios/read_range.cpp`](scenarios/read_range.cpp) | Whole-file reading *vs.* an 80-line range from the same large file. |

## Running

```sh
xmake build -j4 bench-io
xmake run bench-io
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.
