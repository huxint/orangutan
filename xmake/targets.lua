local root = os.projectdir()

local function add_orangutan_common()
    add_includedirs(path.join(root, "src"), {public = true})
    add_packages("cli11", "nlohmann_json", "spdlog", "libcurl", "sqlite3", "cpp-httplib", "stdexec", "rapidhash", "replxx", "mbedtls", "simdutf", "uni_algo", "ctre", "magic_enum", {public = true})
    add_syslinks("pthread", {public = true})
    if has_config("qq_channel") then
        add_defines("ORANGUTAN_ENABLE_QQ_CHANNEL=1", {public = true})
    end
end

target("orangutan-lib")
    set_kind("static")
    add_orangutan_common()
    add_files(
        path.join(root, "src/agent/*.cpp"),
        path.join(root, "src/memory/*.cpp"),
        path.join(root, "src/automation/*.cpp"),
        path.join(root, "src/coordinator/*.cpp"),
        path.join(root, "src/swarm/*.cpp"),
        path.join(root, "src/hooks/*.cpp"),
        path.join(root, "src/skills/*.cpp"),
        path.join(root, "src/heartbeat/*.cpp"),
        path.join(root, "src/channel/*.cpp"),
        path.join(root, "src/channel/qq/*.cpp"),
        path.join(root, "src/web/*.cpp"),
        path.join(root, "src/providers/*.cpp"),
        path.join(root, "src/tools/*.cpp"),
        path.join(root, "src/tools/**/*.cpp"),
        path.join(root, "src/config/*.cpp"),
        path.join(root, "src/storage/*.cpp"),
        path.join(root, "src/process/*.cpp"),
        path.join(root, "src/utils/*.cpp"),
        path.join(root, "src/cli/*.cpp"),
        path.join(root, "src/prompt/*.cpp"),
        path.join(root, "src/bootstrap/*.cpp"),
        path.join(root, "src/permissions/*.cpp")
    )

target("orangutan")
    set_kind("binary")
    add_deps("orangutan-lib")
    add_files(path.join(root, "src/main.cpp"))
