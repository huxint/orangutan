cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER clang)

project(orangutan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(CLI11 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(READLINE REQUIRED readline)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(xxhash)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
    stdexec_source
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG        46ca0458a9806f63cde1c3bba76a2ec720974c00
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(stdexec_source)
if(NOT stdexec_source_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(stdexec_source)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()

option(ORANGUTAN_ENABLE_QQ_CHANNEL "Enable QQ Bot channel support" ON)

if(ORANGUTAN_ENABLE_QQ_CHANNEL)
    FetchContent_Declare(
        boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(boost)
    if(NOT boost_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(PUSH)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(boost)
        if(POLICY CMP0169)
            cmake_policy(POP)
        endif()
    endif()
    file(GLOB BOOST_INCLUDE_DIRS CONFIGURE_DEPENDS "${boost_SOURCE_DIR}/libs/*/include")
endif()

add_library(orangutan_lib STATIC
    src/app/bootstrap.cpp
    src/app/channel-serve.cpp
    src/app/cli-ui.cpp
    src/app/history-events.cpp
    src/app/repl.cpp
    src/app/runtime/app-runtime.cpp
    src/app/runtime/agent-runtime.cpp
    src/app/runtime/identity.cpp
    src/app/runtime/memory-context.cpp
    src/app/session-workflow.cpp
    src/app/slash-commands.cpp
    src/app/single-shot.cpp
    src/core/providers/anthropic-provider.cpp
    src/core/providers/openai-provider.cpp
    src/core/providers/provider-factory.cpp
    src/core/providers/sse-parser.cpp
    src/core/tools/permissions.cpp
    src/core/tools/registry.cpp
    src/features/agent/agent-loop.cpp
    src/features/automation/log-writer.cpp
    src/features/automation/planner.cpp
    src/features/automation/runtime.cpp
    src/features/automation/store.cpp
    src/features/automation/types.cpp
    src/features/channel/core/channel.cpp
    src/features/channel/core/jid-task-runner.cpp
    src/features/channel/qq/channel.cpp
    src/features/channel/qq/transport.cpp
    src/features/cron/parser.cpp
    src/features/heartbeat/protocol/heartbeat-md.cpp
    src/features/heartbeat/protocol/heartbeat-ok.cpp
    src/features/hooks/hook-manager.cpp
    src/features/memory/memory.cpp
    src/features/memory/memory-extract.cpp
    src/features/memory/memory-mirror.cpp
    src/features/memory/memory-schema.cpp
    src/features/memory/memory-search.cpp
    src/features/memory/runtime-memory.cpp
    src/features/skills/skill-loader.cpp
    src/features/subagent/subagent-manager.cpp
    src/features/tools/builtin/automation-tool-support.cpp
    src/features/tools/builtin/heartbeat.cpp
    src/features/tools/builtin/inbox.cpp
    src/features/tools/builtin/register-builtin.cpp
    src/features/tools/builtin/task.cpp
    src/features/tools/core/background-completion.cpp
    src/features/tools/core/command-sandbox.cpp
    src/features/tools/core/edit.cpp
    src/features/tools/core/hashline.cpp
    src/features/tools/core/read.cpp
    src/features/tools/core/register-core.cpp
    src/features/tools/core/shell.cpp
    src/features/tools/core/write.cpp
    src/features/tools/mcp/client.cpp
    src/features/tools/mcp/manager.cpp
    src/features/tools/memory/memory-tools.cpp
    src/features/tools/runtime/runtime-loader.cpp
    src/features/tools/script/script-loader.cpp
    src/features/tools/subagent/subagent-tools.cpp
    src/infra/config/config.cpp
    src/infra/config/config-sections-core.cpp
    src/infra/config/config-sections-integrations.cpp
    src/infra/config/secret-fields.cpp
    src/infra/config/secret-protection-crypto.cpp
    src/infra/config/secret-protection-file.cpp
    src/infra/config/secret-protection-password.cpp
    src/infra/storage/session-store.cpp
    src/infra/storage/sqlite.cpp
    src/infra/storage/subagent-run-store.cpp
    src/features/web/web-server.cpp
    src/features/web/web-routes.cpp
    src/infra/subprocess/subprocess.cpp
)

add_library(orangutan_stdexec INTERFACE)
target_include_directories(orangutan_stdexec SYSTEM INTERFACE ${stdexec_source_SOURCE_DIR}/include)
target_compile_features(orangutan_stdexec INTERFACE cxx_std_20)
target_compile_options(orangutan_stdexec INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fcoroutines>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fconcepts-diagnostics-depth=10>
)

target_include_directories(orangutan_lib PUBLIC src ${SQLITE3_INCLUDE_DIRS} ${xxhash_SOURCE_DIR})

target_link_libraries(orangutan_lib PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    CURL::libcurl
    tomlplusplus::tomlplusplus
    orangutan_stdexec
    Threads::Threads
    OpenSSL::Crypto
    httplib::httplib
    ${SQLITE3_LIBRARIES}
)

if(ORANGUTAN_ENABLE_QQ_CHANNEL)
    target_compile_definitions(orangutan_lib PUBLIC ORANGUTAN_ENABLE_QQ_CHANNEL=1)
    target_compile_definitions(orangutan_lib PUBLIC BOOST_ERROR_CODE_HEADER_ONLY=1)
    target_include_directories(orangutan_lib SYSTEM PUBLIC ${BOOST_INCLUDE_DIRS})
    target_link_libraries(orangutan_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

add_executable(orangutan src/main.cpp)

target_link_libraries(orangutan PRIVATE
    orangutan_lib
    CLI11::CLI11
    ${READLINE_LIBRARIES}
)

target_include_directories(orangutan PRIVATE ${READLINE_INCLUDE_DIRS})

option(BUILD_TESTING "Build tests" ON)

if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    add_executable(orangutan_tests
        tests/app/bootstrap-test.cpp
        tests/app/bootstrap-web-test.cpp
        tests/app/channel-serve-test.cpp
        tests/app/cli-ui-test.cpp
        tests/app/history-events-test.cpp
        tests/app/identity-test.cpp
        tests/app/runtime-agent-runtime-test.cpp
        tests/app/session-workflow-test.cpp
        tests/app/slash-commands-test.cpp
        tests/app/single-shot-test.cpp
        tests/core/provider-fallback-test.cpp
        tests/core/sse-parser-test.cpp
        tests/core/tool-registry-test.cpp
        tests/core/types-test.cpp
        tests/features/agent-loop-test.cpp
        tests/features/automation-runtime-test.cpp
        tests/features/background-shell-completion-test.cpp
        tests/features/automation-store-test.cpp
        tests/features/channel-test.cpp
        tests/features/cron-parser-test.cpp
        tests/features/hashline-test.cpp
        tests/features/heartbeat-tool-test.cpp
        tests/features/heartbeat-md-test.cpp
        tests/features/heartbeat-ok-test.cpp
        tests/features/hook-manager-test.cpp
        tests/features/inbox-tool-test.cpp
        tests/features/jid-task-runner-test.cpp
        tests/features/mcp-client-test.cpp
        tests/features/memory-test.cpp
        tests/features/channel/qq/reconnect-backoff-test.cpp
        tests/features/script-tool-test.cpp
        tests/features/skill-loader-test.cpp
        tests/features/subagent-manager-test.cpp
        tests/features/subagent-tools-test.cpp
        tests/features/task-tool-test.cpp
        tests/features/web-server-test.cpp
        tests/features/web-routes-test.cpp
        tests/features/web-chat-test.cpp
        tests/infra/config-test.cpp
        tests/infra/config-secret-test.cpp
        tests/infra/session-store-test.cpp
        tests/infra/sqlite-test.cpp
        tests/infra/subagent-run-store-test.cpp
        tests/infra/subprocess-test.cpp
        tests/infra/config-save-test.cpp
        tests/integration/hook-integration-test.cpp
        tests/integration/subagent-integration-test.cpp
    )

    target_link_libraries(orangutan_tests PRIVATE
        orangutan_lib
        GTest::gtest
        GTest::gtest_main
        ${READLINE_LIBRARIES}
    )

    target_include_directories(orangutan_tests PRIVATE tests)
    target_compile_definitions(orangutan_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    gtest_discover_tests(orangutan_tests)
endif()

find_program(CLANG_FORMAT clang-format)

if(CLANG_FORMAT)
    file(GLOB_RECURSE ALL_SOURCES
        src/*.cpp src/*.hpp
        tests/*.cpp tests/*.hpp
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on all sources"
    )
endif()

find_program(CLANG_TIDY clang-tidy)

if(CLANG_TIDY)
    file(GLOB_RECURSE TIDY_SOURCES
        src/*.cpp
    )

    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files"
    )
endif()
