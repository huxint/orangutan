# Third-Party Libraries

`xmake/packages.lua` owns version selection. Add a dependency only when existing
code cannot satisfy a concrete requirement; record its purpose, license, boundary
and measured or estimated compile cost here before adding it to the build.

| Library | Version | Consumer and purpose | License | Boundary / cost |
| --- | --- | --- | --- | --- |
| `asio` | 1.36.0 | Async executors, coroutines and cancellation | BSL-1.0 | Executor handles may be public; implementation headers have moderate cost. |
| `libcurl` | >=8.11.0, system | HTTP/SSE transport | curl | Private to `oran-http`; moderate cost. |
| `libqrencode` | 4.1.1, system, optional | Terminal QR encoding for `oran-qq-login` | LGPL-2.1 | Private C API in the opt-in login executable; estimated under 0.1 s/TU. Runtime libraries and tests do not depend on it. |
| `nlohmann_json` | 3.12.0 | Config, tool, provider, memory, channel and bootstrap JSON | MIT | Implementation files only; moderate cost. |
| `re2` | 2025.11.05 | Configuration validation and permission input patterns | BSD-3-Clause | Private compiled regex owner; moderate cost. |
| `sqlite3` | 3.51.0+0 | Storage with FTS5 lexical memory | Public domain | C API private to `oran-storage`; moderate cost. |
| `libsodium` | 1.0.21 | Approval keys, image encoding, QQ signatures and AES-GCM binding credentials | ISC | Private to `oran-permission`, `oran-bootstrap` and channel applications; low cost. |
| `catch2` | 3.7.1 | Behavioral tests | BSL-1.0 | Test targets only; moderate cost. |
| `nanobench` | 4.3.11 | Benchmarks | MIT | Benchmark targets only; low cost. |
| `cmark` | 0.31.2 | Telegram host CommonMark parsing | BSD-2-Clause | Private C API in the host formatter and its tests; low header cost, estimated under 0.1 s/TU. Runtime libraries do not depend on it. |

Use one library per infrastructure responsibility. Keep optional packages out of
default builds. Version changes update this table and receive the same boundary
review as an initial dependency. `make ci` checks manifest/document agreement.

## Opt-In Pure-Vulkan Native Desktop (`orangutan-desktop`)

`apps/desktop-vk/` links directly to system desktop libraries (not pulled into default builds or `oran-*` libraries) and embeds compiled SPIR-V shaders with zero external image assets:

| System Library | Purpose | License |
| --- | --- | --- |
| `libvulkan` (dynamically loaded) | Pure Vulkan 1.0 UI SDF & real-time 3D PBR raymarched companion rendering | Apache-2.0 |
| `SDL3` | Native Wayland/X11 windowing, Vulkan surface creation, HiDPI, clipboard & CJK IME | Zlib |
| `FreeType2` & `Fontconfig` | CJK/Latin/Mono glyph rasterization & system font discovery | FTL / MIT |
| `libpng16` | Headless `--screenshot` PNG export for visual verification | libpng |
