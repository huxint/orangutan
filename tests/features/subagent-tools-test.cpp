#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include "support/ut.hpp"
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
        .runtime_origin = SubagentRuntimeOrigin::cli,
        .raw_caller_id = "cli:local",
    };
}

json parse_tool_json(const ToolResultBlock &result) {
    using namespace boost::ut;

    expect((not result.is_error) >> fatal);
    return json::parse(result.content);
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

boost::ut::suite subagent_tools_suite = [] {
    using namespace boost::ut;

    "parent_runtime_registers_subagent_tools_when_child_agents_allowed"_test = [] {
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
        expect(orangutan::testing::has_tool_named(defs, "subagent_spawn"));
        expect(orangutan::testing::has_tool_named(defs, "subagent_status"));
        expect(orangutan::testing::has_tool_named(defs, "subagent_wait"));
    };

    "child_runtime_does_not_register_subagent_tools"_test = [] {
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
        expect(not orangutan::testing::has_tool_named(defs, "subagent_spawn"));
        expect(not orangutan::testing::has_tool_named(defs, "subagent_status"));
        expect(not orangutan::testing::has_tool_named(defs, "subagent_wait"));
    };

    "incomplete_parent_runtime_context_does_not_register_subagent_tools"_test = [] {
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
        expect(not orangutan::testing::has_tool_named(defs, "subagent_spawn"));
        expect(not orangutan::testing::has_tool_named(defs, "subagent_status"));
        expect(not orangutan::testing::has_tool_named(defs, "subagent_wait"));
    };

    "spawn_rejects_agent_outside_allowlist"_test = [] {
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

        const auto result = registry.execute(ToolUseBlock{
            .id = "spawn-reject",
            .name = "subagent_spawn",
            .input =
                {
                    {"child_agent_key", "coder"},
                    {"child_scope_key", "scope:child"},
                    {"child_session_id", child_session_id},
                    {"task_summary", "Investigate failing parser tests"},
                },
        });

        const auto payload = parse_tool_json(result);
        expect(not payload.at("accepted").get<bool>());
        expect(payload.at("run_id").get<std::string>().empty());
        expect(payload.at("error").get<std::string>().find("not allowed") != std::string::npos);
    };

    "status_and_wait_return_structured_json_responses"_test = [] {
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

        const auto spawn_result = registry.execute(ToolUseBlock{
            .id = "spawn-ok",
            .name = "subagent_spawn",
            .input =
                {
                    {"child_agent_key", "coder"},
                    {"child_scope_key", "scope:child"},
                    {"child_session_id", child_session_id},
                    {"task_summary", "Investigate failing parser tests"},
                },
        });
        const auto spawn_payload = parse_tool_json(spawn_result);
        expect((spawn_payload.at("accepted").get<bool>()) >> fatal);
        const auto run_id = spawn_payload.at("run_id").get<std::string>();
        expect((not run_id.empty()) >> fatal);

        const auto status_result = registry.execute(ToolUseBlock{
            .id = "status-ok",
            .name = "subagent_status",
            .input = {{"run_id", run_id}},
        });
        const auto status_payload = parse_tool_json(status_result);
        expect((status_payload.at("found").get<bool>()) >> fatal);
        expect((not status_payload.at("run").is_null()) >> fatal);
        expect(status_payload.at("run").at("run_id").get<std::string>() == run_id);
        expect(status_payload.at("run").at("child_agent_key").get<std::string>() == "coder");
        expect(status_payload.at("run").at("task_summary").get<std::string>() == "Investigate failing parser tests");

        const auto wait_result = registry.execute(ToolUseBlock{
            .id = "wait-ok",
            .name = "subagent_wait",
            .input =
                {
                    {"run_id", run_id},
                    {"timeout_ms", 1000},
                },
        });
        const auto wait_payload = parse_tool_json(wait_result);
        expect(wait_payload.at("state").get<std::string>() == "completed");
        expect((not wait_payload.at("run").is_null()) >> fatal);
        expect(wait_payload.at("run").at("run_id").get<std::string>() == run_id);
        expect(wait_payload.at("run").at("status").get<std::string>() == "succeeded");
        expect(wait_payload.at("run").at("final_summary").get<std::string>() == "Done");
        expect(wait_payload.at("run").at("final_output").get<std::string>() == "Detailed output");
    };

    "status_and_wait_hide_runs_from_other_runtimes"_test = [] {
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

        const auto spawn_result = parent_registry.execute(ToolUseBlock{
            .id = "spawn-owner",
            .name = "subagent_spawn",
            .input =
                {
                    {"child_agent_key", "coder"},
                    {"child_scope_key", "scope:child"},
                    {"child_session_id", child_session_id},
                    {"task_summary", "Investigate failing parser tests"},
                },
        });
        const auto spawn_payload = parse_tool_json(spawn_result);
        expect((spawn_payload.at("accepted").get<bool>()) >> fatal);
        const auto run_id = spawn_payload.at("run_id").get<std::string>();
        expect((not run_id.empty()) >> fatal);

        auto other_session = parent_session_id;
        auto other_context = make_tool_context(&manager, &other_session, {"coder"});
        other_context.runtime_key = "runtime:cli:other";
        ToolRegistry other_registry;
        register_builtin_tools(other_registry, nullptr, {}, &other_context);

        const auto status_result = other_registry.execute(ToolUseBlock{
            .id = "status-other",
            .name = "subagent_status",
            .input = {{"run_id", run_id}},
        });
        const auto status_payload = parse_tool_json(status_result);
        expect(not status_payload.at("found").get<bool>());
        expect(status_payload.at("run").is_null());

        const auto wait_result = other_registry.execute(ToolUseBlock{
            .id = "wait-other",
            .name = "subagent_wait",
            .input =
                {
                    {"run_id", run_id},
                    {"timeout_ms", 10},
                },
        });
        const auto wait_payload = parse_tool_json(wait_result);
        expect(wait_payload.at("state").get<std::string>() == "not_found");
        expect(wait_payload.at("run").is_null());
    };
};

} // namespace
