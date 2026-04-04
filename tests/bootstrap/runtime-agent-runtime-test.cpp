#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "automation/scheduler.hpp"
#include "automation/automation-store.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "tools/background/background-completion.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace orangutan;
using namespace orangutan::bootstrap;

namespace {

    using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

    const ToolDef *find_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
        const auto it = std::ranges::find_if(definitions, [&name](const ToolDef &definition) {
            return definition.name == name;
        });
        return it == definitions.end() ? nullptr : &(*it);
    }

    std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_test_background_completion_runtime_bindings(const std::shared_ptr<automation::Store> &store,
                                                                                                                BackgroundCompletionResumeCallback resume_callback = {}) {
        return make_background_completion_runtime_bindings(
            [store](const automation::InboxItem &item) {
                static_cast<void>(store->insert_inbox(item));
            },
            std::move(resume_callback));
    }

    class RuntimeAgentRuntimeHarness {
    public:
        RuntimeAgentRuntimeHarness()
        : temp_root_(orangutan::testing::unique_test_root("runtime-agent-runtime")),
          home_root_(temp_root_ / "home"),
          workspace_root_(temp_root_ / "workspace"),
          memory_store_(std::make_unique<MemoryStore>((temp_root_ / "memory.db"))) {
            std::filesystem::create_directories(home_root_ / ".orangutan");
            std::filesystem::create_directories(workspace_root_);
        }

        ~RuntimeAgentRuntimeHarness() {
            memory_store_.reset();
            std::filesystem::remove_all(temp_root_);
        }

        [[nodiscard]]
        AgentRuntimeBuildInput make_input() {
            AgentRuntimeBuildInput input;
            input.primary_endpoint = providers::ProviderEndpoint{
                .profile_name = "test-profile",
                .endpoint_style = "openai-chat-completions",
                .api_key = "test-key",
                .model = "gpt-test",
                .base_url = "https://example.test",
            };
            input.agent_key = "assistant";
            input.workspace_root = workspace_root_.string();
            input.memory = {};
            input.permissions_config = {};
            input.team_agents = {"coder"};
            input.identity = derive_cli_identity(workspace_root_.string(), "assistant");
            input.memory_store = memory_store_.get();
            input.current_session_id = &current_session_id_;
            input.runtime_origin = base::origin::cli;
            input.raw_caller_id = "cli:local";
            return input;
        }

        static void write_skill(const std::filesystem::path &skill_root, const std::string &dir_name, const std::string &skill_name, const std::string &skill_body) {
            const auto skill_dir = skill_root / dir_name;
            std::filesystem::create_directories(skill_dir);
            std::ofstream out(skill_dir / "SKILL.md");
            out << "---\n";
            out << "name: " << skill_name << "\n";
            out << "description: runtime test skill\n";
            out << "---\n\n";
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

    TEST_CASE("builds_runtime_with_memory_and_tools_and_prompt_guidance") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools.definitions();

        REQUIRE(runtime.agent != nullptr);
        REQUIRE(runtime.provider != nullptr);
        REQUIRE(runtime.memory != nullptr);
        CHECK(not(orangutan::testing::has_tool_named(definitions, "memory_list")));
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(orangutan::testing::has_tool_named(definitions, "shell"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_list"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_poll"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_kill"));
        CHECK(runtime.tool_context.background_completion_runtime == nullptr);
        const auto *shell = find_tool_named(definitions, "shell");
        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));
    };

    TEST_CASE("runtime_without_explicit_completion_bindings_does_not_enable_completion_routing") {
        RuntimeAgentRuntimeHarness harness;
        auto automation_store = std::make_shared<automation::Store>((harness.workspace_root() / "automation-no-owner.db"));
        auto automation_runtime = std::make_unique<automation::Runtime>(*automation_store);

        auto input = harness.make_input();
        input.automation_runtime = automation_runtime.get();

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools.definitions();
        const auto *shell = find_tool_named(definitions, "shell");

        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK(runtime.tool_context.background_completion_runtime == nullptr);
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));
    };

    TEST_CASE("runtime_with_explicit_completion_bindings_enables_completion_routing") {
        RuntimeAgentRuntimeHarness harness;
        auto automation_store = std::make_shared<automation::Store>((harness.workspace_root() / "automation-with-owner.db"));
        auto automation_runtime = std::make_unique<automation::Runtime>(*automation_store);
        auto background_completion_runtime = make_test_background_completion_runtime_bindings(automation_store, [](const std::string &) {
            return std::optional<std::string>{};
        });

        auto input = harness.make_input();
        input.automation_runtime = automation_runtime.get();
        input.background_completion_runtime = background_completion_runtime;

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools.definitions();
        const auto *shell = find_tool_named(definitions, "shell");

        CHECK(runtime.tool_context.background_completion_runtime == background_completion_runtime);
        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK(shell->input_schema["properties"].contains("on_complete"));
        CHECK(shell->input_schema["properties"]["on_complete"]["properties"]["mode"]["enum"] == nlohmann::json::array({"inbox", "resume"}));

        input.background_completion_runtime.reset();
        CHECK(runtime.tool_context.background_completion_runtime != nullptr);
        CHECK(runtime.tool_context.background_completion_runtime->supports_completion_routing());
        CHECK(runtime.tool_context.background_completion_runtime->supports_resume_callback());
    };

    TEST_CASE("loads_skills_prompt_from_configured_skill_directory") {
        RuntimeAgentRuntimeHarness harness;
        const auto skill_root = harness.workspace_root() / "skills";
        RuntimeAgentRuntimeHarness::write_skill(skill_root, "delegation", "delegation", "Always report concise delegation status.");

        auto input = harness.make_input();
        input.skill_paths = {skill_root.string()};

        auto runtime = build_agent_runtime(input);

        CHECK(runtime.skills_prompt.contains("## Available Skills"));
        CHECK(runtime.skills_prompt.contains("**delegation**"));
        CHECK_FALSE(runtime.skills_prompt.contains("Always report concise delegation status."));
    };

    TEST_CASE("loads_hooks_from_default_resolved_hook_directories") {
        RuntimeAgentRuntimeHarness harness;
        const auto home_hooks_root = harness.home_root() / ".orangutan" / "hooks";
        const auto workspace_hooks_root = harness.workspace_root() / ".orangutan" / "hooks";
        RuntimeAgentRuntimeHarness::write_executable_hook(home_hooks_root, "before_tool_call", "01-home.sh");
        RuntimeAgentRuntimeHarness::write_executable_hook(workspace_hooks_root, "before_tool_call", "02-workspace.sh");

        ScopedEnvVar home_env("HOME", harness.home_root().string());

        auto input = harness.make_input();
        input.hook_paths.clear();

        auto runtime = build_agent_runtime(input);

        REQUIRE(runtime.hook_manager != nullptr);
        CHECK(runtime.hook_manager->hook_count(HookEvent::before_tool_call) == 2);
        CHECK(runtime.hook_manager->total_hooks() == 2);
    };

    TEST_CASE("keeps_tool_registry_stable_and_permissions_alive_after_move") {
        RuntimeAgentRuntimeHarness harness;
        auto moved_runtime = [&] {
            auto input = harness.make_input();
            input.permissions_config = {};
            input.custom_tools.push_back(Config::ScriptToolConfig{
                .name = "custom_echo",
                .description = "Custom echo script tool",
                .command = "echo hello",
            });

            auto runtime = build_agent_runtime(input);
            CHECK(orangutan::testing::has_tool_named(runtime.tools.definitions(), "custom_echo"));
            const auto *tools_before_move = &runtime.tools;

            auto moved = std::move(runtime);
            CHECK(&moved.tools == tools_before_move);
            return moved;
        }();

        const auto result = moved_runtime.tools.execute(ToolUse("custom-echo", "custom_echo", nlohmann::json::object()));

        CHECK(result.is_error);
        CHECK(result.content.contains("Shell tool blocked by approval policy."));
    };

    TEST_CASE("shared_completion_bindings_remain_usable_after_another_runtime_is_destroyed") {
        RuntimeAgentRuntimeHarness harness;
        auto automation_store = std::make_shared<automation::Store>((harness.workspace_root() / "automation-shared.db"));
        std::size_t resume_callback_count = 0;
        auto shared_bindings = make_test_background_completion_runtime_bindings(automation_store, [&resume_callback_count](const std::string &) {
            ++resume_callback_count;
            return std::optional<std::string>{};
        });

        auto first_input = harness.make_input();
        first_input.background_completion_runtime = shared_bindings;

        auto second_input = harness.make_input();
        second_input.background_completion_runtime = shared_bindings;
        second_input.identity.runtime_key += "|second";

        auto first_runtime = std::make_unique<AgentRuntimeBundle>(build_agent_runtime(first_input));
        auto second_runtime = std::make_unique<AgentRuntimeBundle>(build_agent_runtime(second_input));
        tools::BackgroundCompletionDispatcher dispatcher(&second_runtime->tool_context);

        first_runtime.reset();

        const auto definitions = second_runtime->tools.definitions();
        const auto *shell = find_tool_named(definitions, "shell");
        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK(shell->input_schema["properties"].contains("on_complete"));
        CHECK(dispatcher.supports_resume_callback());

        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-shared",
            .command = "printf 'done\\n'",
            .working_dir = harness.workspace_root().string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(tools::background_completion_mode_metadata_key), "resume"}},
        });

        const auto inbox_items = automation_store->list_inbox(second_input.agent_key);
        CHECK(inbox_items.size() == 1UL);
        CHECK(nlohmann::json::parse(inbox_items.front().body).at("process_id") == "proc-shared");
        CHECK(resume_callback_count == 1UL);
    };

} // namespace
