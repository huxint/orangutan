#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "core/tools/tool.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&](const ToolDef &definition) {
        return definition.name == name;
    });
}

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
    EXPECT_FALSE(result.is_error);
    return json::parse(result.content);
}

} // namespace

class SubagentToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_subagent_tools_test.db";
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    [[nodiscard]]
    std::array<std::string, 2> create_linked_sessions() const {
        SessionStore session_store(db_path().string());
        return {
            session_store.create_empty("test-model", "scope:parent"),
            session_store.create_empty("test-model", "scope:child"),
        };
    }

    [[nodiscard]]
    const std::filesystem::path &db_path() const {
        return db_path_;
    }

private:
    std::filesystem::path db_path_;
};

TEST_F(SubagentToolsTest, ParentRuntimeRegistersSubagentToolsWhenChildAgentsAllowed) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    auto current_session_id = parent_session_id;
    const auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"});

    ToolRegistry registry;
    register_builtin_tools(registry, nullptr, {}, &tool_context);

    const auto defs = registry.definitions();
    EXPECT_TRUE(has_tool_named(defs, "subagent_spawn"));
    EXPECT_TRUE(has_tool_named(defs, "subagent_status"));
    EXPECT_TRUE(has_tool_named(defs, "subagent_wait"));
}

TEST_F(SubagentToolsTest, ChildRuntimeDoesNotRegisterSubagentTools) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    auto current_session_id = parent_session_id;
    const auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"}, true);

    ToolRegistry registry;
    register_builtin_tools(registry, nullptr, {}, &tool_context);

    const auto defs = registry.definitions();
    EXPECT_FALSE(has_tool_named(defs, "subagent_spawn"));
    EXPECT_FALSE(has_tool_named(defs, "subagent_status"));
    EXPECT_FALSE(has_tool_named(defs, "subagent_wait"));
}

TEST_F(SubagentToolsTest, IncompleteParentRuntimeContextDoesNotRegisterSubagentTools) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    auto current_session_id = parent_session_id;
    auto tool_context = make_tool_context(&manager, &current_session_id, {"coder"});
    tool_context.runtime_key.clear();

    ToolRegistry registry;
    register_builtin_tools(registry, nullptr, {}, &tool_context);

    const auto defs = registry.definitions();
    EXPECT_FALSE(has_tool_named(defs, "subagent_spawn"));
    EXPECT_FALSE(has_tool_named(defs, "subagent_status"));
    EXPECT_FALSE(has_tool_named(defs, "subagent_wait"));
}

TEST_F(SubagentToolsTest, SpawnRejectsAgentOutsideAllowlist) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
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
    EXPECT_FALSE(payload.at("accepted").get<bool>());
    EXPECT_TRUE(payload.at("run_id").get<std::string>().empty());
    EXPECT_NE(payload.at("error").get<std::string>().find("not allowed"), std::string::npos);
}

TEST_F(SubagentToolsTest, StatusAndWaitReturnStructuredJsonResponses) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
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
    ASSERT_TRUE(spawn_payload.at("accepted").get<bool>());
    const auto run_id = spawn_payload.at("run_id").get<std::string>();
    ASSERT_FALSE(run_id.empty());

    const auto status_result = registry.execute(ToolUseBlock{
        .id = "status-ok",
        .name = "subagent_status",
        .input = {{"run_id", run_id}},
    });
    const auto status_payload = parse_tool_json(status_result);
    EXPECT_TRUE(status_payload.at("found").get<bool>());
    EXPECT_EQ(status_payload.at("run").at("run_id").get<std::string>(), run_id);
    EXPECT_EQ(status_payload.at("run").at("child_agent_key").get<std::string>(), "coder");
    EXPECT_EQ(status_payload.at("run").at("task_summary").get<std::string>(), "Investigate failing parser tests");

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
    EXPECT_EQ(wait_payload.at("state").get<std::string>(), "completed");
    EXPECT_EQ(wait_payload.at("run").at("run_id").get<std::string>(), run_id);
    EXPECT_EQ(wait_payload.at("run").at("status").get<std::string>(), "succeeded");
    EXPECT_EQ(wait_payload.at("run").at("final_summary").get<std::string>(), "Done");
    EXPECT_EQ(wait_payload.at("run").at("final_output").get<std::string>(), "Detailed output");
}

TEST_F(SubagentToolsTest, StatusAndWaitHideRunsFromOtherRuntimes) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
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
    ASSERT_TRUE(spawn_payload.at("accepted").get<bool>());
    const auto run_id = spawn_payload.at("run_id").get<std::string>();

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
    EXPECT_FALSE(status_payload.at("found").get<bool>());
    EXPECT_TRUE(status_payload.at("run").is_null());

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
    EXPECT_EQ(wait_payload.at("state").get<std::string>(), "not_found");
    EXPECT_TRUE(wait_payload.at("run").is_null());
}
