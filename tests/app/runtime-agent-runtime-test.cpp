#include "app/runtime/agent-runtime.hpp"

#include "app/runtime/identity.hpp"
#include "features/hooks/hook-manager.hpp"
#include "features/memory/memory.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace orangutan;

namespace {

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&name](const ToolDef &definition) {
        return definition.name == name;
    });
}

class RuntimeAgentRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = std::filesystem::current_path() / "tmp" / "tests" / "runtime-agent-runtime";
        home_root_ = temp_root_ / "home";
        workspace_root_ = temp_root_ / "workspace";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(home_root_ / ".orangutan");
        std::filesystem::create_directories(workspace_root_);

        memory_store_ = std::make_unique<MemoryStore>((temp_root_ / "memory.db").string());
    }

    void TearDown() override {
        memory_store_.reset();
        std::filesystem::remove_all(temp_root_);
    }

    [[nodiscard]]
    AgentRuntimeBuildInput make_input() {
        AgentRuntimeBuildInput input;
        input.provider_name = "openai";
        input.api_key = "test-key";
        input.model = "gpt-test";
        input.base_url = "https://example.test";
        input.agent_key = "assistant";
        input.system_prompt = "You are a runtime bootstrap test agent.";
        input.workspace_root = workspace_root_.string();
        input.memory = {};
        input.permissions = {};
        input.allowed_child_agents = {"coder"};
        input.identity = derive_cli_identity(workspace_root_.string(), "assistant");
        input.memory_store = memory_store_.get();
        input.current_session_id = &current_session_id_;
        input.runtime_origin = SubagentRuntimeOrigin::cli;
        input.raw_caller_id = "cli:local";
        return input;
    }

    static void write_skill(const std::filesystem::path &skill_root, const std::string &dir_name, const std::string &skill_name, const std::string &skill_body) {
        const auto skill_dir = skill_root / dir_name;
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "+++\n";
        out << "name = \"" << skill_name << "\"\n";
        out << "description = \"runtime test skill\"\n";
        out << "+++\n\n";
        out << skill_body << "\n";
    }

    static void write_executable_hook(const std::filesystem::path &hooks_root, const std::string &event_name, const std::string &filename) {
        const auto event_dir = hooks_root / event_name;
        std::filesystem::create_directories(event_dir);
        const auto hook_path = event_dir / filename;

        std::ofstream out(hook_path);
        out << "#!/bin/sh\n";
        out << "exit 0\n";
        out.close();

        std::filesystem::permissions(hook_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    [[nodiscard]]
    const std::filesystem::path &home_root() const {
        return home_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

private:
    std::filesystem::path temp_root_;
    std::filesystem::path home_root_;
    std::filesystem::path workspace_root_;
    std::unique_ptr<MemoryStore> memory_store_;
    std::string current_session_id_;
};

TEST_F(RuntimeAgentRuntimeTest, BuildsRuntimeWithMemoryAndToolsAndPromptGuidance) {
    auto input = make_input();

    auto runtime = build_agent_runtime(input);

    ASSERT_NE(runtime.agent, nullptr);
    ASSERT_NE(runtime.provider, nullptr);
    ASSERT_NE(runtime.memory, nullptr);
    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "memory_list"));
    EXPECT_TRUE(has_tool_named(runtime.tools.definitions(), "shell"));
    EXPECT_NE(runtime.system_prompt.find("subagent"), std::string::npos);
    EXPECT_NE(runtime.system_prompt.find("subagent_spawn"), std::string::npos);
}

TEST_F(RuntimeAgentRuntimeTest, LoadsSkillsPromptFromConfiguredSkillDirectory) {
    const auto skill_root = workspace_root() / "skills";
    write_skill(skill_root, "delegation", "delegation", "Always report concise delegation status.");

    auto input = make_input();
    input.skill_paths = {skill_root.string()};

    auto runtime = build_agent_runtime(input);

    EXPECT_NE(runtime.skills_prompt.find("## Active Skills"), std::string::npos);
    EXPECT_NE(runtime.skills_prompt.find("### delegation"), std::string::npos);
    EXPECT_NE(runtime.skills_prompt.find("Always report concise delegation status."), std::string::npos);
}

TEST_F(RuntimeAgentRuntimeTest, LoadsHooksFromDefaultResolvedHookDirectories) {
    const auto home_hooks_root = home_root() / ".orangutan" / "hooks";
    const auto workspace_hooks_root = workspace_root() / ".orangutan" / "hooks";
    write_executable_hook(home_hooks_root, "before_tool_call", "01-home.sh");
    write_executable_hook(workspace_hooks_root, "before_tool_call", "02-workspace.sh");

    ScopedEnvVar home_env("HOME", home_root().string());

    auto input = make_input();
    input.hook_paths.clear();

    auto runtime = build_agent_runtime(input);

    ASSERT_NE(runtime.hook_manager, nullptr);
    EXPECT_EQ(runtime.hook_manager->hook_count(HookEvent::before_tool_call), 2);
    EXPECT_EQ(runtime.hook_manager->total_hooks(), 2);
}

} // namespace
