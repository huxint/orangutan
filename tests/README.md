# Tests

One Catch2 target per library: `test-<lib>`. Core integration lives in
`tests/bootstrap`; it uses controlled providers with real temporary storage.
`test-helpers/run_async.hpp` drives awaitables with a hard timeout.

Run `xmake test -j4` for all buckets, or build and run `test-<lib>` while iterating.
For a case filter, invoke the built test executable directly. See
[testing-and-bench](../docs/rules/testing-and-bench.md).
