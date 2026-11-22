-- xmake/packages.lua — third-party requirements.
--
-- Every package listed here must also appear in docs/rules/libraries.md
-- (docs/rules/docs-in-sync.md — Check 6 in scripts/check-docs-sync.sh).
-- Packages land with the library that introduces them, so the default build
-- only pays for dependencies that current targets actually consume.

add_requires("asio 1.36.0")
add_requires("catch2 3.7.1")
add_requires("cmark 0.31.2")
add_requires("libcurl >=8.11.0", { system = true })
add_requires("libqrencode 4.1.1", { system = true, optional = true })
add_requires("libsodium 1.0.21")
add_requires("nanobench 4.3.11")
add_requires("nlohmann_json 3.12.0")
add_requires("re2 2025.11.05")
add_requires("sqlite3 3.51.0+0", { configs = { cflags = "-DSQLITE_ENABLE_FTS5" } })
