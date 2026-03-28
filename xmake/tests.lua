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

add_test_target("test-app", rooted("tests/app/*.cpp"))

add_test_target("test-core", rooted("tests/core/*.cpp"))

add_test_target("test-infra-config", rooted("tests/infra/config-*.cpp"))

add_test_target("test-infra-storage", {
    rooted("tests/infra/session-store-test.cpp"),
    rooted("tests/infra/sqlite-test.cpp"),
    rooted("tests/infra/subagent-run-store-test.cpp"),
})

add_test_target("test-infra-subprocess", rooted("tests/infra/subprocess-test.cpp"))

add_test_target("test-features-tools", {
    rooted("tests/features/background-shell-completion-test.cpp"),
    rooted("tests/features/hashline-test.cpp"),
    rooted("tests/features/heartbeat-tool-test.cpp"),
    rooted("tests/features/inbox-tool-test.cpp"),
    rooted("tests/features/mcp-client-test.cpp"),
    rooted("tests/features/script-tool-test.cpp"),
    rooted("tests/features/task-tool-test.cpp"),
})

add_test_target("test-features-memory", {
    rooted("tests/features/memory-test.cpp"),
    rooted("tests/features/automation-runtime-test.cpp"),
    rooted("tests/features/automation-store-test.cpp"),
})

add_test_target("test-features-web", {
    rooted("tests/features/web-server-test.cpp"),
    rooted("tests/features/web-routes-test.cpp"),
    rooted("tests/features/web-chat-test.cpp"),
})

add_test_target("test-features-channel", {
    rooted("tests/features/channel-test.cpp"),
    rooted("tests/features/jid-task-runner-test.cpp"),
    rooted("tests/features/channel/qq/reconnect-backoff-test.cpp"),
})

add_test_target("test-features-misc", {
    rooted("tests/features/agent-loop-test.cpp"),
    rooted("tests/features/cron-parser-test.cpp"),
    rooted("tests/features/heartbeat-md-test.cpp"),
    rooted("tests/features/heartbeat-ok-test.cpp"),
    rooted("tests/features/hook-manager-test.cpp"),
    rooted("tests/features/skill-loader-test.cpp"),
    rooted("tests/features/subagent-manager-test.cpp"),
    rooted("tests/features/subagent-tools-test.cpp"),
})

add_test_target("test-integration", rooted("tests/integration/*.cpp"))
