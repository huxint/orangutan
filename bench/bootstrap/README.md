# Bootstrap Benchmark

Run `xmake build bench-bootstrap` then `xmake run bench-bootstrap`.

| Scenario | Comparison |
| --- | --- |
| [`runtime_assembly_build.cpp`](scenarios/runtime_assembly_build.cpp) | Runtime assembly with storage-backed audit enabled and disabled, using the same workspace and memory configuration. |
| [`permissions.cpp`](scenarios/permissions.cpp) | Compile default rules alone, then with an eight-rule global configuration and a two-rule agent overlay. |
