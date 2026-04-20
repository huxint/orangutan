local root = os.projectdir()

local function rooted(pattern)
    return path.join(root, pattern)
end

local function add_test_target(name, files)
    target(name)
        set_kind("binary")
        add_deps("orangutan-lib")
        add_packages("catch2")
        add_includedirs(rooted("src"), rooted("tests"))
        add_defines(format('SOURCE_DIR="%s"', root))
        add_files(files)
        add_tests("default")
end

add_test_target("test-types", rooted("tests/types/*.cpp"))

add_test_target("test-providers", rooted("tests/providers/**/*.cpp"))

add_test_target("test-tools", {
    rooted("tests/tools/**/*.cpp"),
})

add_test_target("test-agent", rooted("tests/agent/*.cpp"))

add_test_target("test-memory", rooted("tests/memory/*.cpp"))

add_test_target("test-automation", rooted("tests/automation/*.cpp"))

add_test_target("test-orchestration", rooted("tests/orchestration/*.cpp"))

add_test_target("test-heartbeat", rooted("tests/heartbeat/*.cpp"))

add_test_target("test-channel", {
    rooted("tests/channel/*.cpp"),
    rooted("tests/channel/qq/*.cpp"),
})

add_test_target("test-misc-services", {
    rooted("tests/hooks/*.cpp"),
    rooted("tests/skills/*.cpp"),
})

add_test_target("test-cli", rooted("tests/cli/*.cpp"))

add_test_target("test-web", rooted("tests/web/*.cpp"))

add_test_target("test-config", rooted("tests/config/*.cpp"))

add_test_target("test-storage", rooted("tests/storage/*.cpp"))

add_test_target("test-process", rooted("tests/process/*.cpp"))

add_test_target("test-permissions", rooted("tests/permissions/*.cpp"))

add_test_target("test-bootstrap", rooted("tests/bootstrap/*.cpp"))

add_test_target("test-utils", rooted("tests/utils/*.cpp"))

add_test_target("test-integration", rooted("tests/integration/*.cpp"))
