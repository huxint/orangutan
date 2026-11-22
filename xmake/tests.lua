-- xmake/tests.lua — Catch2 v3 buckets.
--
-- One target per tests/<lib>/ directory. The convention is documented in
-- docs/rules/testing-and-bench.md.

local root = os.projectdir()

local function oran_test(name, deps, extra_packages)
    target("test-" .. name)
        set_kind("binary")
        set_group("oran-tests")
        set_default(false)
        add_includedirs(path.join(root, "include"), { public = false })
        add_files(path.join(root, "tests", name, "**.cpp"))
        if name == "bootstrap" then
            add_files(path.join(root, "eval/context/scenarios.cpp"), path.join(root, "eval/context/runner.cpp"))
            add_files(path.join(root, "apps/telegram/host.cpp"))
            add_files(path.join(root, "apps/telegram/state.cpp"))
            add_files(path.join(root, "apps/telegram/format.cpp"), path.join(root, "apps/telegram/presentation.cpp"))
            add_files(path.join(root, "apps/telegram/input.cpp"))
            add_files(path.join(root, "apps/telegram/commands.cpp"))
            add_files(path.join(root, "apps/qq/login.cpp"), path.join(root, "apps/qq/chat.cpp"))
        end
        add_deps(table.unpack(deps))
        add_packages("catch2")
        if extra_packages then
            for _, pkg in ipairs(extra_packages) do
                add_packages(pkg)
            end
        end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
        add_tests("default", { runargs = { "--reporter=console", "--verbosity=normal" } })
        on_run(function (target)
            os.execv(target:targetfile(), { "--reporter=console", "--verbosity=normal" })
        end)
    target_end()
end

oran_test("core", { "oran-core" })
oran_test("async", { "oran-async" })
oran_test("http", { "oran-http" })
oran_test("io", { "oran-io" })
oran_test("storage", { "oran-storage" })
oran_test("config", { "oran-config" })
oran_test("permission", { "oran-permission" })
oran_test("hook", { "oran-hook" })
oran_test("memory", { "oran-memory" })
oran_test("tool", { "oran-tool" }, { "nlohmann_json" })
oran_test("prompt", { "oran-prompt" })
oran_test("provider", { "oran-provider" }, { "nlohmann_json" })
oran_test("agent", { "oran-agent" })
oran_test("bootstrap", { "oran-bootstrap" }, { "nlohmann_json", "cmark", "libsodium" })

oran_test("channel", { "oran-channel" }, { "nlohmann_json" })
