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

## Optional Desktop Prototype

`apps/desktop/package.json` and its npm lockfile own frontend versions. These
packages are private to the browser prototype, add no C++ includes or compile
cost, and are not installed or built by xmake. Expected frontend checks/build
take seconds; browser installation is a separate development-only download.

| Package | Version | Purpose | License |
| --- | --- | --- | --- |
| `react`, `react-dom` | 19.3.0 | UI and browser rendering | MIT |
| `lucide-react` | 1.49.0 | Consistent SVG interface icons | ISC |
| `react-markdown` | 10.1.0 | Safe Markdown rendering without raw HTML execution | MIT |
| `remark-gfm` | 4.0.1 | Markdown tables and task lists in artifacts | MIT |
| `zod` | 4.6.5 | Validate persisted demo state before use | MIT |
| `vite` | 8.3.1 | Development server and production bundle | MIT |
| `@vitejs/plugin-react` | 6.1.1 | React JSX and development refresh | MIT |
| `typescript` | 7.0.2 | Static type checking, development only | Apache-2.0 |
| `@types/react`, `@types/react-dom` | 19.3.0 | React type declarations, development only | MIT |
| `@playwright/test` | 1.63.0 | Browser interaction tests and screenshots, development only | Apache-2.0 |
| `prettier` | 3.9.9 | Consistent frontend source formatting, development only | MIT |
