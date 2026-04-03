#include "storage/session-store.hpp"
#include "storage/subagent-run-store.hpp"
#include "subagent/subagent-manager.hpp"
#include "tools/registry/tool.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
using namespace orangutan;

namespace {

    ToolRuntimeContext make_tool_context(SubagentManager *manager, std::string *current_session_id, std::vector<std::string> allowed_child_agents, bool is_child_run = false) {
        return ToolRuntimeContext{
            .runtime_key = "runtime:cli:default",
            .agent_key = "default",
            .scope_key = "scope:parent",
            .current_session_id = current_session_id,
            .allowed_child_agents = std::move(allowed_child_agents),
            .is_child_run = is_child_run,
            .subagent_manager = manager,
            .runtime_origin = base::origin::cli,
            .raw_caller_id = "cli:local",
        };
    }

    nlohmann::json parse_tool_json(const ToolResult &result) {

        REQUIRE(not result.is_error);
        return nlohmann::json::parse(result.content);
    }

    class SubagentToolsHarness {
    public:
        SubagentToolsHarness()
        : db_path_(orangutan::testing::unique_test_db_path("subagent-tools", "subagents.db")) {}

        ~SubagentToolsHarness() {
            std::filesystem::remove_all(db_path_.parent_path());
        }

        [[nodiscard]]
        std::array<std::string, 2> create_linked_sessions() const {
            SessionStore session_store(db_path());
            return {
                session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
                session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:child", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
            };
        }

        [[nodiscard]]
        const std::filesystem::path &db_path() const {
            return db_path_;
        }

    private:
        std::filesystem::path db_path_;
    };

    TEST_CASE("parent_runtime_registers_subagent_tools_when_child_agents_allowed") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        auto current_session_id = parent_session_id;
        const auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"});

        ToolRegistry registry;
        register_builtin_tools(registry, nullptr, {}, &tool_context);

        const auto defs = registry.definitions();
        CHECK(not(orangutan::testing::has_tool_named(defs, "subagent_spawn")));
        CHECK(not(orangutan::testing::has_tool_named(defs, "subagent_status")));
        CHECK(not(orangutan::testing::has_tool_named(defs, "subagent_wait")));
        // Subagent tools are deferred but registered and executable
        CHECK(registry.find_definition("subagent_spawn") != nullptr);
        CHECK(registry.find_definition("subagent_status") != nullptr);
        CHECK(registry.find_definition("subagent_wait") != nullptr);
    };

    TEST_CASE("child_runtime_does_not_register_subagent_tools") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        auto current_session_id = parent_session_id;
        const auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"}, true);

        ToolRegistry registry;
        register_builtin_tools(registry, nullptr, {}, &tool_context);

        const auto defs = registry.definitions();
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_spawn"));
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_status"));
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_wait"));
    };

    TEST_CASE("incomplete_parent_runtime_context_does_not_register_subagent_tools") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        auto current_session_id = parent_session_id;
        auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"});
        tool_context.runtime_key.clear();

        ToolRegistry registry;
        register_builtin_tools(registry, nullptr, {}, &tool_context);

        const auto defs = registry.definitions();
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_spawn"));
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_status"));
        CHECK_FALSE(orangutan::testing::has_tool_named(defs, "subagent_wait"));
    };

    TEST_CASE("spawn_rejects_agent_outside_allowlist") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });
        auto current_session_id = parent_session_id;
        const auto tool_context = make_tool_context(&manager, &current_session_id, {"reviewer"});

        ToolRegistry registry;
        register_builtin_tools(registry, nullptr, {}, &tool_context);

        const auto result = registry.execute(ToolUse("spawn-reject", "subagent_spawn",
                                                     {
                                                         {"child_agent_key", "coder"},
                                                         {"child_scope_key", "scope:child"},
                                                         {"child_session_id", child_session_id},
                                                         {"task_summary", "Investigate failing parser tests"},
                                                     }));

        const auto payload = parse_tool_json(result);
        CHECK_FALSE(payload.at("accepted").get<bool>());
        CHECK(payload.at("run_id").get<std::string>().empty());
        CHECK(payload.at("error").get<std::string>().contains("not allowed"));
    };

    TEST_CASE("status_and_wait_return_structured_json_responses") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{
                .status = SubagentRunStatus::succeeded,
                .final_summary = "Done",
                .final_output = "Detailed output",
            };
        });
        auto current_session_id = parent_session_id;
        const auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"});

        ToolRegistry registry;
        register_builtin_tools(registry, nullptr, {}, &tool_context);

        const auto spawn_result = registry.execute(ToolUse("spawn-ok", "subagent_spawn",
                                                           {
                                                               {"child_agent_key", "coder"},
                                                               {"child_scope_key", "scope:child"},
                                                               {"child_session_id", child_session_id},
                                                               {"task_summary", "Investigate failing parser tests"},
                                                           }));
        const auto spawn_payload = parse_tool_json(spawn_result);
        REQUIRE(spawn_payload.at("accepted").get<bool>());
        const auto run_id = spawn_payload.at("run_id").get<std::string>();
        REQUIRE(not run_id.empty());

        const auto status_result = registry.execute(ToolUse("status-ok", "subagent_status", {{"run_id", run_id}}));
        const auto status_payload = parse_tool_json(status_result);
        REQUIRE(status_payload.at("found").get<bool>());
        REQUIRE(not status_payload.at("run").is_null());
        CHECK(status_payload.at("run").at("run_id").get<std::string>() == run_id);
        CHECK(status_payload.at("run").at("child_agent_key").get<std::string>() == "coder");
        CHECK(status_payload.at("run").at("task_summary").get<std::string>() == "Investigate failing parser tests");

        const auto wait_result = registry.execute(ToolUse("wait-ok", "subagent_wait",
                                                          {
                                                              {"run_id", run_id},
                                                              {"timeout_ms", 1000},
                                                          }));
        const auto wait_payload = parse_tool_json(wait_result);
        CHECK(wait_payload.at("state").get<std::string>() == "completed");
        REQUIRE(not wait_payload.at("run").is_null());
        CHECK(wait_payload.at("run").at("run_id").get<std::string>() == run_id);
        CHECK(wait_payload.at("run").at("status").get<std::string>() == "succeeded");
        CHECK(wait_payload.at("run").at("final_summary").get<std::string>() == "Done");
        CHECK(wait_payload.at("run").at("final_output").get<std::string>() == "Detailed output");
    };

    TEST_CASE("status_and_wait_hide_runs_from_other_runtimes") {
        SubagentToolsHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path());
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{
                .status = SubagentRunStatus::succeeded,
                .final_summary = "Done",
                .final_output = "Detailed output",
            };
        });

        auto parent_session = parent_session_id;
        const auto parent_context = make_tool_context(&manager, &parent_session, {"coder"});
        ToolRegistry parent_registry;
        register_builtin_tools(parent_registry, nullptr, {}, &parent_context);

        const auto spawn_result = parent_registry.execute(ToolUse("spawn-owner", "subagent_spawn",
                                                                  {
                                                                      {"child_agent_key", "coder"},
                                                                      {"child_scope_key", "scope:child"},
                                                                      {"child_session_id", child_session_id},
                                                                      {"task_summary", "Investigate failing parser tests"},
                                                                  }));
        const auto spawn_payload = parse_tool_json(spawn_result);
        REQUIRE(spawn_payload.at("accepted").get<bool>());
        const auto run_id = spawn_payload.at("run_id").get<std::string>();
        REQUIRE(not run_id.empty());

        auto other_session = parent_session_id;
        auto other_context = make_tool_context(&manager, &other_session, {"coder"});
        other_context.runtime_key = "runtime:cli:other";
        ToolRegistry other_registry;
        register_builtin_tools(other_registry, nullptr, {}, &other_context);

        const auto status_result = other_registry.execute(ToolUse("status-other", "subagent_status", {{"run_id", run_id}}));
        const auto status_payload = parse_tool_json(status_result);
        CHECK_FALSE(status_payload.at("found").get<bool>());
        CHECK(status_payload.at("run").is_null());

        const auto wait_result = other_registry.execute(ToolUse("wait-other", "subagent_wait",
                                                                {
                                                                    {"run_id", run_id},
                                                                    {"timeout_ms", 10},
                                                                }));
        const auto wait_payload = parse_tool_json(wait_result);
        CHECK(wait_payload.at("state").get<std::string>() == "not_found");
        CHECK(wait_payload.at("run").is_null());
    };

} // namespace
