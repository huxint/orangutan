local root = os.projectdir()

local function add_orangutan_common()
    add_includedirs(path.join(root, "src"), {public = true})
    add_packages("cli11", "nlohmann_json", "spdlog", "libcurl", "sqlite3", "toml++", "cpp-httplib", "stdexec", "rapidhash", "replxx", "mbedtls", "simdutf", "uni_algo", "ctre", "magic_enum", {public = true})
    add_syslinks("pthread", {public = true})
    if has_config("qq_channel") then
        add_defines("ORANGUTAN_ENABLE_QQ_CHANNEL=1", {public = true})
    end
end

target("orangutan-lib")
    set_kind("static")
    add_orangutan_common()
    add_files(
        path.join(root, "src/core/**/*.cpp"),
        path.join(root, "src/infra/*.cpp"),
        path.join(root, "src/infra/**/*.cpp"),
        path.join(root, "src/features/**/*.cpp"),
        path.join(root, "src/app/*.cpp"),
        path.join(root, "src/app/runtime/*.cpp")
    )

target("orangutan")
    set_kind("binary")
    add_deps("orangutan-lib")
    add_files(path.join(root, "src/main.cpp"))
