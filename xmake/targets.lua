-- Production library targets follow docs/ARCHITECTURE.md.

local root = os.projectdir()

local function oran_lib(name, deps, private_packages, public_packages)
    target("oran-" .. name)
        set_kind("static")
        set_group("oran-libs")
        add_includedirs(path.join(root, "include"), { public = true })
        add_files(path.join(root, "src", "oran-" .. name, "**.cpp"))
        if deps then
            add_deps(table.unpack(deps))
        end
        if private_packages then
            for _, pkg in ipairs(private_packages) do
                add_packages(pkg, { public = false })
            end
        end
        if public_packages then
            for _, pkg in ipairs(public_packages) do
                add_packages(pkg, { public = true })
            end
        end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_lib("core", {}, {})
oran_lib("async", { "oran-core" }, {}, { "asio" })
oran_lib("http", { "oran-core", "oran-async" }, { "libcurl" }, { "asio" })
oran_lib("io", { "oran-core", "oran-async" }, {}, { "asio" })
oran_lib("storage", { "oran-core", "oran-async" }, { "sqlite3" })
oran_lib("config", { "oran-core" }, { "nlohmann_json", "re2" })
oran_lib("permission", { "oran-core", "oran-storage", "oran-async" }, { "re2", "libsodium" })
oran_lib("hook", { "oran-core", "oran-async" }, {})
oran_lib("memory", { "oran-core", "oran-async", "oran-storage" }, { "nlohmann_json" })
oran_lib("tool", { "oran-core", "oran-async", "oran-io", "oran-permission", "oran-hook" }, { "nlohmann_json" })
oran_lib("prompt", { "oran-core" }, {})
oran_lib("provider", { "oran-core", "oran-async" }, { "nlohmann_json" })
oran_lib("agent", { "oran-core", "oran-async", "oran-storage", "oran-prompt", "oran-tool", "oran-provider", "oran-hook" }, {})

oran_lib("channel", { "oran-core", "oran-async", "oran-permission", "oran-hook" }, { "nlohmann_json" })

oran_lib("bootstrap", { "oran-core", "oran-async", "oran-http", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-memory", "oran-tool", "oran-provider", "oran-agent", "oran-channel" }, { "nlohmann_json", "libsodium" })
