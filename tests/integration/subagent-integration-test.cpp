#include "features/agent/agent-loop.hpp"
#include "app/runtime/identity.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include "support/ut.hpp"
using namespace orangutan;

namespace {

class ScriptedProvider final : public Provider {
public:
    using Step = std::function<LLMResponse(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &)>;

    explicit ScriptedProvider(std::vector<Step> steps)
    : steps_(std::move(steps)) {}

    LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &, int) override {
        if (next_step_ >= steps_.size()) {
            throw std::runtime_error("no scripted response available");
        }
        return steps_[next_step_++](system_prompt, messages, tools);
    }

    std::string name() const override {
        return "scripted-provider";
    }

private:
    std::vector<Step> steps_;
    size_t next_step_ = 0;
};

const ToolResultBlock *last_tool_result(const std::vector<Message> &history) {
    for (const auto &message : std::ranges::reverse_view(history)) {
        for (const auto &block : std::ranges::reverse_view(message.content)) {
            if (const auto *result = std::get_if<ToolResultBlock>(&block); result != nullptr) {
                return result;
            }
        }
    }
    return nullptr;
}

struct SubagentIntegrationHarness {
    SubagentIntegrationHarness()
    : workspace_root_(orangutan::testing::unique_test_root("subagent-integration-workspace")),
      db_path_(orangutan::testing::unique_test_db_path("subagent-integration", "runs.db")) {}

    ~SubagentIntegrationHarness() {
        std::filesystem::remove_all(workspace_root_);
        std::filesystem::remove_all(db_path_.parent_path());
    }

    SubagentIntegrationHarness(const SubagentIntegrationHarness &) = delete;
    SubagentIntegrationHarness &operator=(const SubagentIntegrationHarness &) = delete;
    SubagentIntegrationHarness(SubagentIntegrationHarness &&) = delete;
    SubagentIntegrationHarness &operator=(SubagentIntegrationHarness &&) = delete;

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &db_path() const {
        return db_path_;
    }

    [[nodiscard]]
    static ToolRuntimeContext make_parent_tool_context(SubagentManager &manager, std::string &current_session_id) {
        return ToolRuntimeContext{
            .runtime_key = derive_cli_runtime_key("default"),
            .agent_key = "default",
            .scope_key = derive_cli_session_scope("default"),
            .current_session_id = &current_session_id,
            .allowed_child_agents = {"coder"},
            .is_child_run = false,
            .subagent_manager = &manager,
            .runtime_origin = SubagentRuntimeOrigin::cli,
            .raw_caller_id = "cli:local",
        };
    }

private:
    std::filesystem::path workspace_root_;
    std::filesystem::path db_path_;
};

boost::ut::suite subagent_integration_suite = [] {
    using namespace boost::ut;

    "parent_can_spawn_and_wait_for_real_child_run_with_isolated_transcript"_test = [] {
        SubagentIntegrationHarness harness;
        SessionStore session_store(harness.db_path());
        SubagentRunStore run_store(harness.db_path());
        const auto parent_session_id = session_store.create_empty(
            orangutan::SessionMetadata{.model = "parent-model", .scope_key = derive_cli_session_scope("default"), .agent_key = "", .origin_kind = "cli", .origin_ref = ""});

        std::vector<std::string> parent_prompts;
        std::vector<std::string> child_prompts;

        std::unordered_map<std::string, SubagentChildRuntimeConfig> child_configs;
        child_configs.emplace("coder", SubagentChildRuntimeConfig{
                                           .agent_key = "coder",
                                           .provider_name = "child-provider",
                                           .api_key = "unused",
                                           .model = "child-model",
                                           .base_url = "https://example.test",
                                           .system_prompt = "Child base prompt.",
                                           .workspace_root = (harness.workspace_root() / "child-root").string(),
                                           .allowed_child_agents = {"reviewer"},
                                       });

        SubagentManager manager(
            run_store,
            SubagentExecutionEnvironment{
                .agent_configs = &child_configs,
                .session_store = &session_store,
                .memory_store = nullptr,
                .provider_factory =
                    [&](const SubagentChildRuntimeConfig &config) {
                        expect(config.agent_key == "coder");
                        std::vector<ScriptedProvider::Step> steps;
                        steps.reserve(2);
                        steps.emplace_back([&](std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools) -> LLMResponse {
                            child_prompts.emplace_back(system_prompt);
                            expect(system_prompt.contains("delegated worker"));
                            expect(system_prompt.contains("cannot spawn subagents"));
                            expect(not orangutan::testing::has_tool_named(tools, "subagent_spawn"));
                            expect(messages.size() == 1_ul);
                            const auto *text = std::get_if<TextBlock>(&messages[0].content[0]);
                            expect((text != nullptr) >> fatal);
                            expect(text->text == "Investigate parser regression");
                            return LLMResponse{
                                .stop_reason = "tool_use",
                                .content = {ToolUseBlock{.id = "child-write", .name = "write", .input = {{"path", "child-notes.txt"}, {"content", "child touched its workspace"}}}},
                            };
                        });
                        steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                            return LLMResponse{
                                .stop_reason = "end_turn",
                                .content = {TextBlock{.text = "child completed delegated work"}},
                            };
                        });
                        return std::make_unique<ScriptedProvider>(std::move(steps));
                    },
            });

        auto current_session_id = parent_session_id;
        auto tool_context = SubagentIntegrationHarness::make_parent_tool_context(manager, current_session_id);

        ToolRegistry parent_tools;
        register_builtin_tools(parent_tools, nullptr, harness.workspace_root().string(), &tool_context);

        std::vector<ScriptedProvider::Step> parent_steps;
        parent_steps.reserve(3);
        parent_steps.emplace_back([&](std::string_view system_prompt, const std::vector<Message> &, const std::vector<ToolDef> &tools) -> LLMResponse {
            parent_prompts.emplace_back(system_prompt);
            expect(system_prompt.contains("Use `subagent_spawn`"));
            expect(system_prompt.contains("Use `subagent_wait`"));
            expect(orangutan::testing::has_tool_named(tools, "subagent_spawn"));
            expect(orangutan::testing::has_tool_named(tools, "subagent_wait"));
            return LLMResponse{
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "spawn-1", .name = "subagent_spawn", .input = {{"child_agent_key", "coder"}, {"task_summary", "Investigate parser regression"}}}},
            };
        });
        parent_steps.emplace_back([&](std::string_view, const std::vector<Message> &messages, const std::vector<ToolDef> &) -> LLMResponse {
            const auto *spawn_result = last_tool_result(messages);
            expect((spawn_result != nullptr) >> fatal);
            const auto payload = json::parse(spawn_result->content);
            expect((payload.at("accepted").get<bool>()) >> fatal);
            return LLMResponse{
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "wait-1", .name = "subagent_wait", .input = {{"run_id", payload.at("run_id")}, {"timeout_ms", 1000}}}},
            };
        });
        parent_steps.emplace_back([&](std::string_view, const std::vector<Message> &messages, const std::vector<ToolDef> &) -> LLMResponse {
            const auto *wait_result = last_tool_result(messages);
            expect((wait_result != nullptr) >> fatal);
            const auto payload = json::parse(wait_result->content);
            expect(payload.at("state").get<std::string>() == "completed");
            expect(payload.at("run").at("status").get<std::string>() == "succeeded");
            expect(payload.at("run").at("final_output").get<std::string>() == "child completed delegated work");
            return LLMResponse{
                .stop_reason = "end_turn",
                .content = {TextBlock{.text = "parent finished after child run"}},
            };
        });
        ScriptedProvider parent_provider(std::move(parent_steps));

        const auto parent_prompt = append_subagent_prompt_guidance("Parent base prompt.", {"coder"}, false);
        AgentLoop parent_loop(parent_provider, parent_tools, parent_prompt, nullptr, derive_cli_session_scope("default"));

        const auto final_output = parent_loop.run("Handle the parser issue");
        expect(final_output == "parent finished after child run");

        session_store.update(parent_session_id, parent_loop.history());

        expect(parent_prompts.size() == 1_ul);
        expect(child_prompts.size() == 1_ul);

        const auto *parent_wait_result = last_tool_result(parent_loop.history());
        expect((parent_wait_result != nullptr) >> fatal);
        const auto parent_wait_payload = json::parse(parent_wait_result->content);

        const auto wait_run_result = manager.wait(SubagentWaitRequest{
            .run_id = parent_wait_payload.at("run").at("run_id").get<std::string>(),
            .timeout = std::chrono::milliseconds{1},
            .caller =
                SubagentCallerContext{
                    .runtime_origin = SubagentRuntimeOrigin::cli,
                    .runtime_key = derive_cli_runtime_key("default"),
                    .agent_key = "default",
                    .scope_key = derive_cli_session_scope("default"),
                    .raw_caller_id = "cli:local",
                    .session_id = parent_session_id,
                    .allowed_child_agents = {"coder"},
                    .is_child_run = false,
                },
        });
        expect(wait_run_result.state == SubagentWaitState::completed);
        expect((wait_run_result.run.has_value()) >> fatal);

        const auto expected_child_identity = derive_child_identity((harness.workspace_root() / "child-root").string(), "cli:local", "coder");
        expect(wait_run_result.run->child_scope_key == expected_child_identity.memory_scope);
        expect(std::filesystem::exists(std::filesystem::path(expected_child_identity.workspace) / "child-notes.txt"));

        const auto child_history = session_store.load(wait_run_result.run->child_session_id);
        expect(child_history.size() == 4_ul);
        expect(child_history[0].role == Role::User);
        expect(std::get<TextBlock>(child_history[0].content[0]).text == "Investigate parser regression");
        expect(std::get<TextBlock>(child_history[3].content[0]).text == "child completed delegated work");

        const auto parent_history = session_store.load(parent_session_id);
        expect((not parent_history.empty()) >> fatal);
        expect(std::get<TextBlock>(parent_history[0].content[0]).text == "Handle the parser issue");
        for (const auto &message : parent_history) {
            for (const auto &block : message.content) {
                if (const auto *text = std::get_if<TextBlock>(&block); text != nullptr) {
                    expect(not text->text.contains("child completed delegated work"));
                }
            }
        }
    };

    "child_run_uses_channel_caller_origin_for_scope_and_workspace"_test = [] {
        SubagentIntegrationHarness harness;
        SessionStore session_store(harness.db_path());
        SubagentRunStore run_store(harness.db_path());

        std::unordered_map<std::string, SubagentChildRuntimeConfig> child_configs;
        child_configs.emplace("coder", SubagentChildRuntimeConfig{
                                           .agent_key = "coder",
                                           .provider_name = "child-provider",
                                           .api_key = "unused",
                                           .model = "child-model",
                                           .base_url = "https://example.test",
                                           .system_prompt = "Child base prompt.",
                                           .workspace_root = (harness.workspace_root() / "channel-child-root").string(),
                                       });

        SubagentManager manager(
            run_store,
            SubagentExecutionEnvironment{
                .agent_configs = &child_configs,
                .session_store = &session_store,
                .memory_store = nullptr,
                .provider_factory =
                    [&](const SubagentChildRuntimeConfig &) {
                        std::vector<ScriptedProvider::Step> steps;
                        steps.reserve(2);
                        steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                            return LLMResponse{
                                .stop_reason = "tool_use",
                                .content = {ToolUseBlock{.id = "write-1", .name = "write", .input = {{"path", "channel-child.txt"}, {"content", "channel child workspace"}}}},
                            };
                        });
                        steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                            return LLMResponse{
                                .stop_reason = "end_turn",
                                .content = {TextBlock{.text = "channel child done"}},
                            };
                        });
                        return std::make_unique<ScriptedProvider>(std::move(steps));
                    },
            });

        const auto spawn_result = manager.spawn(SubagentSpawnRequest{
            .caller =
                SubagentCallerContext{
                    .runtime_origin = SubagentRuntimeOrigin::channel,
                    .runtime_key = "agent:default|jid:qqbot:c2c:alice",
                    .agent_key = "default",
                    .scope_key = "agent:default|jid:qqbot:c2c:alice",
                    .raw_caller_id = "qqbot:c2c:alice",
                    .session_id = std::nullopt,
                    .allowed_child_agents = {"coder"},
                    .is_child_run = false,
                },
            .child_agent_key = "coder",
            .task_summary = "Inspect incoming channel issue",
        });
        expect((spawn_result.accepted) >> fatal);

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
            .caller =
                SubagentCallerContext{
                    .runtime_origin = SubagentRuntimeOrigin::channel,
                    .runtime_key = "agent:default|jid:qqbot:c2c:alice",
                    .agent_key = "default",
                    .scope_key = "agent:default|jid:qqbot:c2c:alice",
                    .raw_caller_id = "qqbot:c2c:alice",
                    .allowed_child_agents = {"coder"},
                },
        });
        expect(wait_result.state == SubagentWaitState::completed);
        expect((wait_result.run.has_value()) >> fatal);

        const auto expected_identity = derive_child_identity((harness.workspace_root() / "channel-child-root").string(), "qqbot:c2c:alice", "coder");
        expect(wait_result.run->child_scope_key == expected_identity.memory_scope);
        expect(std::filesystem::exists(std::filesystem::path(expected_identity.workspace) / "channel-child.txt"));
    };
};

} // namespace
