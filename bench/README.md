# Benchmarks

Each library has a `bench-<lib>` target. Compare alternatives under the same
workload, compiler and machine; report measurement limits. Run the selected
bucket with `xmake build bench-<lib>` followed by `xmake run bench-<lib>`.

[Testing and benchmarking](../docs/rules/testing-and-bench.md) owns the rules.
