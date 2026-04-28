#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/agent-loop-teammate.hpp"
#include "bootstrap/app-runtime.hpp"
#include "bootstrap/automation-executor.hpp"
#include "bootstrap/identity.hpp"
#include "bootstrap/runtime-control.hpp"
#include "bootstrap/runtime-factory.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "permissions/permission-state.hpp"
#include "tools/background/background-completion.hpp"
#include "test-provider-support.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
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

    std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_test_background_completion_runtime_bindings(automation::AutomationService *service,
                                                                                                                 BackgroundCompletionResumeCallback resume_callback = {}) {
        return make_background_completion_runtime_bindings(
            [service](const automation::DeliveryRecord &delivery) {
                if (service != nullptr) {
                    static_cast<void>(service->record_delivery(delivery));
                }
            },
            std::move(resume_callback));
    }

    class CapturingOpenAiServer {
    public:
        CapturingOpenAiServer() {
            server_.Post("/v1/chat/completions", [this](const httplib::Request &request, httplib::Response &response) {
                {
                    std::scoped_lock lock(mutex_);
                    requests_.push_back(nlohmann::json::parse(request.body));
                }
                response.set_content(
                    "data: {\"choices\":[{\"delta\":{\"content\":\"teammate done\"},\"finish_reason\":null}]}\n\n"
                    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
                    "text/event-stream");
            });
            port_ = server_.bind_to_any_port("127.0.0.1");
            if (port_ <= 0) {
                throw std::runtime_error("failed to bind capturing provider server");
            }
            server_thread_ = std::jthread([this] {
                server_.listen_after_bind();
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!server_.is_running() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (!server_.is_running()) {
                server_.stop();
                throw std::runtime_error("capturing provider server did not start");
            }
        }

        ~CapturingOpenAiServer() {
            server_.stop();
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }

        CapturingOpenAiServer(const CapturingOpenAiServer &) = delete;
        CapturingOpenAiServer &operator=(const CapturingOpenAiServer &) = delete;
        CapturingOpenAiServer(CapturingOpenAiServer &&) = delete;
        CapturingOpenAiServer &operator=(CapturingOpenAiServer &&) = delete;

        [[nodiscard]]
        std::string base_url() const {
            return "http://127.0.0.1:" + std::to_string(port_);
        }

        [[nodiscard]]
        std::vector<nlohmann::json> requests() const {
            std::scoped_lock lock(mutex_);
            return requests_;
        }

    private:
        httplib::Server server_;
        int port_ = 0;
        mutable std::mutex mutex_;
        std::vector<nlohmann::json> requests_;
        std::jthread server_thread_;
    };

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
        RuntimeAgentRuntimeHarness(const RuntimeAgentRuntimeHarness &) = delete;
        RuntimeAgentRuntimeHarness &operator=(const RuntimeAgentRuntimeHarness &) = delete;
        RuntimeAgentRuntimeHarness(RuntimeAgentRuntimeHarness &&) = delete;
        RuntimeAgentRuntimeHarness &operator=(RuntimeAgentRuntimeHarness &&) = delete;

        [[nodiscard]]
        AgentRuntimeBuildInput make_input() {
            AgentRuntimeBuildInput input;
            input.provider_route = providers::ProviderRoute{
                .primary =
                    {
                        .profile_name = "test-profile",
                        .model = "gpt-test",
                        .base_url = "https://example.test",
                        .api_key = "test-key",
                        .provider = providers::provider_kind::openai,
                        .protocol = providers::protocol_kind::chat_completions,
                    },
            };
            input.agent_key = "assistant";
            input.workspace_root = workspace_root_.string();
            input.permission_context = {};
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
        const auto definitions = runtime.tools().definitions();

        REQUIRE(runtime.agent != nullptr);
        REQUIRE(runtime.provider != nullptr);
        REQUIRE(runtime.memory != nullptr);
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(orangutan::testing::has_tool_named(definitions, "shell"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_list"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_poll"));
        CHECK(orangutan::testing::has_tool_named(definitions, "process_kill"));
        CHECK(runtime.tool_context().background_completion_runtime == nullptr);
        const auto *shell = find_tool_named(definitions, "shell");
        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));
    };

    TEST_CASE("runtime_factory_builds_cli_runtime_with_identity_permissions_and_loaders") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        Config app_cfg;
        std::string current_session_id = "cli-session";
        ToolPermissionContext permission_context;
        permission_context.mode = permission_mode::plan;

        AgentConfig agent_cfg{
            .model = "gpt-test",
            .fallback_models = {FallbackModelRef{"gpt-fallback"}, FallbackModelRef{"backup", "gpt-backup"}},
            .workspace = harness.workspace_root().string(),
            .thinking_budget = 128,
        };
        auto runtime_cfg = make_agent_runtime_config("assistant", agent_cfg, input.provider_route, harness.workspace_root().string(), permission_context);
        const auto identity = derive_cli_identity(harness.workspace_root().string(), "assistant");

        auto runtime = build_runtime_bundle(RuntimeFactoryRequest{
            .runtime_config = &runtime_cfg,
            .identity = &identity,
            .app_config = &app_cfg,
            .memory_store = input.memory_store,
            .current_session_id = &current_session_id,
            .runtime_origin = base::origin::cli,
            .raw_caller_id = "cli:local",
        });

        CHECK(runtime_cfg.fallback_models == std::vector<std::string>{"gpt-fallback", "backup:gpt-backup"});
        CHECK(runtime.tool_context().runtime_key == identity.runtime_key);
        CHECK(runtime.tool_context().scope_key == identity.memory_scope);
        CHECK(runtime.tool_context().runtime_origin == base::origin::cli);
        CHECK(runtime.tool_context().raw_caller_id == "cli:local");
        CHECK(runtime.tool_context().current_session_id == &current_session_id);
        REQUIRE(runtime.tool_context().permission_context != nullptr);
        CHECK(runtime.tool_context().permission_context == &runtime.permissions());
        CHECK(runtime.permissions().mode == permission_mode::plan);
        REQUIRE(runtime.provider != nullptr);
        REQUIRE(runtime.skill_loader != nullptr);
        REQUIRE(runtime.agent != nullptr);
    };

    TEST_CASE("runtime_factory_borrows_hook_manager_without_taking_ownership") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        Config app_cfg;
        HookManager shared_hooks;
        AgentConfig agent_cfg{
            .model = "gpt-test",
            .workspace = harness.workspace_root().string(),
        };
        auto runtime_cfg = make_agent_runtime_config("assistant", agent_cfg, input.provider_route, harness.workspace_root().string(), ToolPermissionContext{});
        const auto identity = derive_cli_identity(harness.workspace_root().string(), "assistant");

        auto runtime = build_runtime_bundle(RuntimeFactoryRequest{
            .runtime_config = &runtime_cfg,
            .identity = &identity,
            .app_config = &app_cfg,
            .memory_store = input.memory_store,
            .current_session_id = input.current_session_id,
            .hook_manager = &shared_hooks,
        });

        CHECK(runtime.hook_manager == nullptr);
        CHECK(runtime.active_hook_manager() == &shared_hooks);
    };

    TEST_CASE("runtime_factory_builds_automation_runtime_with_cli_origin_and_completion_bindings") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        Config app_cfg;
        bootstrap::AppRuntime app_runtime(harness.workspace_root() / "automation-factory.db");
        std::string current_session_id;
        AgentConfig agent_cfg{
            .model = "gpt-test",
            .workspace = harness.workspace_root().string(),
        };
        auto runtime_cfg = make_agent_runtime_config("assistant", agent_cfg, input.provider_route, harness.workspace_root().string(), ToolPermissionContext{});
        const RuntimeIdentity identity{
            .workspace = runtime_cfg.workspace_root,
            .runtime_key = "agent:assistant|automation:job-1",
            .memory_scope = "agent:assistant|automation",
        };
        auto completion_resume_state = std::make_shared<RuntimeCompletionResumeState>();
        completion_resume_state->agent_key = runtime_cfg.agent_key;
        completion_resume_state->configured_model = runtime_cfg.model;
        completion_resume_state->scope_key = identity.memory_scope;
        completion_resume_state->automation_runtime = &app_runtime.automation_runtime();
        completion_resume_state->suppress_human_output = true;
        auto background_completion_runtime = make_runtime_background_completion_bindings(&app_runtime.automation_runtime(), make_runtime_completion_resume_callback(completion_resume_state));

        auto runtime = build_runtime_bundle(RuntimeFactoryRequest{
            .runtime_config = &runtime_cfg,
            .identity = &identity,
            .app_config = &app_cfg,
            .memory_store = input.memory_store,
            .current_session_id = &current_session_id,
            .runtime_origin = base::origin::cli,
            .raw_caller_id = identity.runtime_key,
            .automation_service = &app_runtime.automation_service(),
            .automation_runtime = &app_runtime.automation_runtime(),
            .background_completion_runtime = background_completion_runtime,
        });

        CHECK(runtime.tool_context().runtime_origin == base::origin::cli);
        CHECK(runtime.tool_context().raw_caller_id == identity.runtime_key);
        CHECK(runtime.tool_context().current_session_id == &current_session_id);
        CHECK(runtime.tool_context().automation_service == &app_runtime.automation_service());
        CHECK(runtime.tool_context().automation_runtime == &app_runtime.automation_runtime());
        CHECK(runtime.tool_context().background_completion_runtime == background_completion_runtime);
        REQUIRE(runtime.tool_context().background_completion_runtime != nullptr);
        CHECK(runtime.tool_context().background_completion_runtime->supports_completion_routing());
        CHECK(runtime.tool_context().background_completion_runtime->supports_resume_callback());
        CHECK(completion_resume_state->suppress_human_output);
    };

    TEST_CASE("bootstrap_automation_executor_reports_missing_agent_without_building_runtime") {
        RuntimeAgentRuntimeHarness harness;
        bootstrap::AppRuntime app_runtime(harness.workspace_root() / "automation-missing-agent.db");
        Config cfg;
        const std::unordered_map<std::string, AgentRuntimeConfig> runtime_configs;
        auto input = harness.make_input();

        const auto result = execute_automation_with_runtime(automation::Automation{
                                                                .id = "job-1",
                                                                .agent_key = "missing",
                                                                .name = "missing-agent",
                                                                .prompt = "run task",
                                                            },
                                                            AutomationExecutorDependencies{
                                                                .config = &cfg,
                                                                .agent_runtime_configs = &runtime_configs,
                                                                .memory_store = input.memory_store,
                                                                .automation_runtime = &app_runtime.automation_runtime(),
                                                            });

        CHECK_FALSE(result.success);
        CHECK(result.summary == "No runtime configuration for agent 'missing'.");
        CHECK(result.reply.empty());
        CHECK(result.workspace_root.empty());
    };

    TEST_CASE("standalone_runtime_with_orchestration_exposes_teammate_capability_without_team_allowlist") {
        RuntimeAgentRuntimeHarness harness;
        orchestration::OrchestrationManager orchestration_manager(2);

        auto input = harness.make_input();
        input.orchestration_manager = &orchestration_manager;

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools().definitions();

        CHECK(orangutan::testing::has_tool_named(definitions, "agent_spawn"));
        CHECK(orangutan::testing::has_tool_named(definitions, "agent_send_message"));
        CHECK(orangutan::testing::has_tool_named(definitions, "agent_stop"));
        CHECK(orangutan::testing::has_tool_named(definitions, "team_create"));
        CHECK(orangutan::testing::has_tool_named(definitions, "team_delete"));
        CHECK(runtime.skills_prompt.contains("## Agent Orchestration"));
        CHECK(runtime.skills_prompt.contains("persistent teammates"));
        CHECK(runtime.skills_prompt.contains("no preset teammate catalog"));
        CHECK(runtime.skills_prompt.contains("Use `tool_search` when you need specific MCP tools."));
        CHECK(runtime.skills_prompt.contains("Long-term memory is loaded for scope"));

        orchestration_manager.shutdown();
    };

    TEST_CASE("deferred_tools_remain_directly_executable_before_discovery") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        input.permission_context.mode = permission_mode::bypass_permissions;

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools().definitions();

        CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "remember"));
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(runtime.tools().has_deferred_tools());

        const auto remember = runtime.tools().execute(ToolUse("remember-before-discovery", "remember", {{"key", "runtime.theme"}, {"content", "amber"}}));
        CHECK_FALSE(remember.is_error);

        const auto recall = runtime.tools().execute(ToolUse("recall-before-discovery", "recall", {{"query", "runtime.theme"}}));
        CHECK_FALSE(recall.is_error);
        CHECK(recall.content.contains("amber"));
    };

    TEST_CASE("leader_mode_runtime_exposes_only_orchestration_tools") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        input.agent_role = orchestration::agent_role::leader;

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools().definitions();

        CHECK(definitions.size() == 5);
        CHECK(orangutan::testing::has_tool_named(definitions, "agent_spawn"));
        CHECK(orangutan::testing::has_tool_named(definitions, "agent_send_message"));
        CHECK(orangutan::testing::has_tool_named(definitions, "agent_stop"));
        CHECK(orangutan::testing::has_tool_named(definitions, "team_create"));
        CHECK(orangutan::testing::has_tool_named(definitions, "team_delete"));
        CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "shell"));
        CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "file_read"));
        CHECK_FALSE(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(runtime.skills_prompt.contains("You are a leader agent"));
        CHECK(runtime.skills_prompt.contains("Teammate Relationships"));
        CHECK(runtime.skills_prompt.contains("Tool Workflow"));
    };

    TEST_CASE("child_runtime_uses_teammate_prompt_guidance") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        input.agent_role = orchestration::agent_role::teammate;
        input.delegated_task_prompt = "Inspect the logging pipeline and report defects.";

        auto runtime = build_agent_runtime(input);

        CHECK(runtime.skills_prompt.contains("You are a teammate named assistant"));
        CHECK(runtime.skills_prompt.contains("Inspect the logging pipeline and report defects."));
        CHECK(runtime.skills_prompt.contains("Focus exclusively on the task assigned to you."));
        CHECK(runtime.skills_prompt.contains("agent_send_message"));
    };

    TEST_CASE("formats_idle_teammate_mailbox_messages_as_one_prompt") {
        const std::vector<orchestration::MailboxMessage> messages{
            {
                .from = "leader<main>",
                .text = "first & task",
            },
            {
                .from = "peer\"two\"",
                .text = "second <update>",
            },
        };

        const auto prompt = detail::format_teammate_messages_prompt(messages);

        CHECK(prompt == R"(<teammate-message from="leader&lt;main&gt;">first &amp; task</teammate-message>
<teammate-message from="peer&quot;two&quot;">second &lt;update&gt;</teammate-message>)");
    };

    TEST_CASE("agent_loop_teammate_factory_builds_teammate_prompt_and_receives_followups") {
        RuntimeAgentRuntimeHarness harness;
        CapturingOpenAiServer server;
        Config cfg;
        MemoryStore memory_store((harness.workspace_root() / "teammate-memory.db"));
        orchestration::OrchestrationManager orchestration_manager(1);
        orchestration::TeamManager team_manager(harness.workspace_root() / "teams.db");
        orchestration::AgentMailbox mailbox(harness.workspace_root() / "mailbox.db");
        const auto team = team_manager.create_team("runtime-team", "Runtime test team", "agent:default");

        AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .provider_route = testing::make_test_route("gpt-test", providers::provider_kind::openai, providers::protocol_kind::chat_completions, "test-key", server.base_url()),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, AgentRuntimeConfig> runtime_configs{{"default", runtime_cfg}};
        auto factory = make_agent_loop_teammate_factory(cfg, runtime_configs, &memory_store, orchestration_manager, &team_manager, &mailbox);

        auto teammate = factory(orchestration::AgentSpawnRequest{
            .name = "reviewer",
            .task = "Audit bootstrap runtime assembly.",
            .team_id = team.id,
            .config_agent_key = "default",
        });
        REQUIRE(teammate != nullptr);
        CHECK(teammate->can_receive_followups());

        CHECK(teammate->run("Initial spawn prompt", std::stop_token{}) == "teammate done");

        const auto requests = server.requests();
        REQUIRE(requests.size() == 1UL);
        const auto system_message = requests.front().at("messages").at(0).at("content").get<std::string>();
        CHECK(system_message.contains("You are a teammate named reviewer"));
        CHECK(system_message.contains("Audit bootstrap runtime assembly."));
        CHECK(system_message.contains("Focus exclusively on the task assigned to you."));

        mailbox.send(team.id, "lead<main>", "reviewer", "follow & verify");
        auto followup = teammate->poll_next_prompt();
        REQUIRE(followup.has_value());
        CHECK(*followup == R"(<teammate-message from="lead&lt;main&gt;">follow &amp; verify</teammate-message>)");
        CHECK_FALSE(teammate->poll_next_prompt().has_value());

        orchestration_manager.shutdown();
    };

    TEST_CASE("runtime_without_explicit_completion_bindings_does_not_enable_completion_routing") {
        RuntimeAgentRuntimeHarness harness;
        bootstrap::AppRuntime app_runtime(harness.workspace_root() / "automation-no-owner.db");

        auto input = harness.make_input();
        input.automation_service = &app_runtime.automation_service();
        input.automation_runtime = &app_runtime.automation_runtime();

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools().definitions();
        const auto *shell = find_tool_named(definitions, "shell");

        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK(runtime.tool_context().background_completion_runtime == nullptr);
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));
    };

    TEST_CASE("runtime_with_explicit_completion_bindings_enables_completion_routing") {
        RuntimeAgentRuntimeHarness harness;
        bootstrap::AppRuntime app_runtime(harness.workspace_root() / "automation-with-owner.db");
        auto background_completion_runtime = make_test_background_completion_runtime_bindings(&app_runtime.automation_service(), [](const std::string &) {
            return std::optional<std::string>{};
        });

        auto input = harness.make_input();
        input.automation_service = &app_runtime.automation_service();
        input.automation_runtime = &app_runtime.automation_runtime();
        input.background_completion_runtime = background_completion_runtime;

        auto runtime = build_agent_runtime(input);
        const auto definitions = runtime.tools().definitions();
        const auto *shell = find_tool_named(definitions, "shell");

        CHECK(runtime.tool_context().background_completion_runtime == background_completion_runtime);
        REQUIRE(shell != nullptr);
        CHECK(shell->input_schema.contains("properties"));
        CHECK(shell->input_schema["properties"].contains("on_complete"));
        CHECK(shell->input_schema["properties"]["on_complete"]["properties"]["mode"]["enum"] == nlohmann::json::array({"inbox", "resume"}));

        input.background_completion_runtime.reset();
        CHECK(runtime.tool_context().background_completion_runtime != nullptr);
        CHECK(runtime.tool_context().background_completion_runtime->supports_completion_routing());
        CHECK(runtime.tool_context().background_completion_runtime->supports_resume_callback());
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

    TEST_CASE("runtime_uses_no_skills_fallback_when_catalog_is_empty") {
        RuntimeAgentRuntimeHarness harness;
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        auto input = harness.make_input();

        auto runtime = build_agent_runtime(input);

        CHECK(runtime.skills_prompt.contains("## Available Skills"));
        CHECK(runtime.skills_prompt.contains("No skills are currently loaded in this runtime."));
        CHECK(runtime.skills_prompt.contains("Do not invent skill names; answer explicit skill-list questions from this fact."));
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
        CHECK(runtime.hook_manager->hook_count(hook_event::before_tool_call) == 2);
        CHECK(runtime.hook_manager->total_hooks() == 2);
    };

    TEST_CASE("runtime_initializes_permission_context_with_workspace_settings_and_cli_overrides") {
        RuntimeAgentRuntimeHarness harness;
        const auto orangutan_dir = harness.workspace_root() / ".orangutan";
        std::filesystem::create_directories(orangutan_dir);

        {
            std::ofstream out(orangutan_dir / "settings.json");
            out << R"json({"permissions":{"allow":["shell(git:*)"]}})json";
        }
        {
            std::ofstream out(orangutan_dir / "settings.local.json");
            out << R"json({"permissions":{"deny":["edit"]}})json";
        }

        auto input = harness.make_input();
        input.permission_context = initialize_permission_context(
            PermissionConfig{
                .default_mode = permission_mode::accept_edits,
                .allow = {"read"},
            },
            CLIPermissionOptions{
                .mode_override = permission_mode::plan,
                .allowed_tools = {"task(list)"},
            },
            harness.workspace_root());

        auto runtime = build_agent_runtime(input);
        const auto *permission_context = runtime.tool_context().permission_context;

        REQUIRE(permission_context != nullptr);
        CHECK(permission_context->mode == permission_mode::plan);
        CHECK(std::ranges::any_of(permission_context->allow_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "read" && rule.source == permission_rule_source::user_settings;
        }));
        CHECK(std::ranges::any_of(permission_context->allow_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "shell" && rule.source == permission_rule_source::project_settings && rule.content.has_value() && rule.content->pattern == "git";
        }));
        CHECK(std::ranges::any_of(permission_context->allow_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "task" && rule.source == permission_rule_source::cli_arg && rule.content.has_value() && rule.content->pattern == "list";
        }));
        CHECK(std::ranges::any_of(permission_context->deny_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "edit" && rule.source == permission_rule_source::local_settings;
        }));
    };

    TEST_CASE("replace_permissions_updates_live_tool_visibility_and_execution") {
        RuntimeAgentRuntimeHarness harness;
        auto input = harness.make_input();
        input.permission_context.mode = permission_mode::plan;

        auto runtime = build_agent_runtime(input);

        CHECK_FALSE(orangutan::testing::has_tool_named(runtime.tools().definitions(), "shell"));

        runtime.replace_permissions(change_mode(runtime.permissions(), permission_mode::bypass_permissions));

        CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "shell"));
        const auto shell_result = runtime.tools().execute(ToolUse("runtime-replaced-shell", "shell", {{"command", "echo hello"}}));
        CHECK_FALSE(shell_result.is_error);
        CHECK(shell_result.content.contains("hello"));
    };

    TEST_CASE("keeps_tool_registry_stable_and_permissions_alive_after_move") {
        RuntimeAgentRuntimeHarness harness;
        auto moved_runtime = [&] {
            auto input = harness.make_input();
            input.permission_context = {};
            input.custom_tools.push_back(Config::ScriptToolConfig{
                .name = "custom_echo",
                .description = "Custom echo script tool",
                .command = "echo hello",
            });

            auto runtime = build_agent_runtime(input);
            CHECK(orangutan::testing::has_tool_named(runtime.tools().definitions(), "custom_echo"));
            const auto *tools_before_move = &runtime.tools();

            auto moved = std::move(runtime);
            CHECK(&moved.tools() == tools_before_move);
            return moved;
        }();

        const auto result = moved_runtime.tools().execute(ToolUse("custom-echo", "custom_echo", nlohmann::json::object()));

        CHECK_FALSE(result.is_error);
        CHECK(result.content.contains("hello"));
    };

    TEST_CASE("shared_completion_bindings_remain_usable_after_another_runtime_is_destroyed") {
        RuntimeAgentRuntimeHarness harness;
        bootstrap::AppRuntime app_runtime(harness.workspace_root() / "automation-shared.db");
        std::size_t resume_callback_count = 0;
        auto shared_bindings = make_test_background_completion_runtime_bindings(&app_runtime.automation_service(), [&resume_callback_count](const std::string &) {
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
        tools::BackgroundCompletionDispatcher dispatcher(&second_runtime->tool_context());

        first_runtime.reset();

        const auto definitions = second_runtime->tools().definitions();
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
            .terminal_status = background_process_terminal_status::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(tools::BACKGROUND_COMPLETION_MODE_METADATA_KEY), "resume"}},
        });

        const auto deliveries = app_runtime.automation_service().list_deliveries(automation::DeliveryQuery{.agent_key = second_input.agent_key});
        CHECK(deliveries.size() == 1UL);
        CHECK(nlohmann::json::parse(deliveries.front().body).at("process_id") == "proc-shared");
        CHECK(resume_callback_count == 1UL);
    };

} // namespace
