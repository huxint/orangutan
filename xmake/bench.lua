-- xmake/bench.lua — nanobench buckets.
--
-- One target per bench/<lib>/ directory; see docs/rules/testing-and-bench.md.

local root = os.projectdir()

local function oran_bench(name, deps)
    target("bench-" .. name)
        set_kind("binary")
        set_group("oran-benches")
        set_default(false)
        add_includedirs(path.join(root, "include"), { public = false })
        add_files(path.join(root, "bench", name, "**.cpp"))
        add_deps(table.unpack(deps))
        add_packages("nanobench")
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_bench("core", { "oran-core" })
oran_bench("async", { "oran-async" })
oran_bench("http", { "oran-http" })
oran_bench("io", { "oran-io" })
oran_bench("storage", { "oran-storage" })
oran_bench("config", { "oran-config" })
oran_bench("permission", { "oran-permission" })
oran_bench("hook", { "oran-hook" })
oran_bench("memory", { "oran-memory" })
oran_bench("tool", { "oran-tool", "oran-hook" })
oran_bench("prompt", { "oran-prompt" })
oran_bench("provider", { "oran-provider" })
oran_bench("agent", { "oran-agent" })
oran_bench("bootstrap", { "oran-bootstrap" })

oran_bench("channel", { "oran-channel" })
