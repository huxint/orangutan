# Public Headers

Each library exposes narrow headers under `oran/<lib>/` and an umbrella header
at `oran/<lib>.hpp`. The [architecture](../docs/ARCHITECTURE.md#public-headers)
lists the current interfaces.

Use narrow includes when possible. Heavy dependencies stay in implementation
files, as required by [critical rule C6](../docs/rules/critical-rules.md#c6-public-headers-contain-no-heavy-includes).
The shared PCH is `oran/_pch.hpp`; [compile guidance](../docs/FAST_COMPILATION.md)
explains how to measure changes.
