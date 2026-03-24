#include "app/session-workflow.hpp"

#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include "support/ut.hpp"

using namespace orangutan;

namespace {

class DistillingWorkflowProvider final : public Provider {
public:
    LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "memory|project|project.current|0.8|orangutan refactor\n"
                                          "journal|Reviewed session decisions"}},
        };
    }

    LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used");
    }

    std::string name() const override {
        return "workflow-provider";
    }
};

class SessionWorkflowHarness {
public:
    SessionWorkflowHarness()
    : temp_root_(orangutan::testing::unique_test_root("session-workflow")),
      session_db_path_(temp_root_ / "sessions.db"),
      memory_db_path_(temp_root_ / "memory.db") {}

    ~SessionWorkflowHarness() {
        std::filesystem::remove_all(temp_root_);
    }

    [[nodiscard]]
    const std::filesystem::path &session_db_path() const {
        return session_db_path_;
    }

    [[nodiscard]]
    const std::filesystem::path &memory_db_path() const {
        return memory_db_path_;
    }

private:
    std::filesystem::path temp_root_;
    std::filesystem::path session_db_path_;
    std::filesystem::path memory_db_path_;
};

boost::ut::suite session_workflow_suite = [] {
    using namespace boost::ut;

    "start_new_session_distills_and_persists_previous_history"_test = [] {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        MemoryStore memory_store(harness.memory_db_path());
        RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{.scope = "scope:test"});
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools, {}, &runtime_memory);

        loop.set_history({
            Message::user_text("we are working on orangutan refactor"),
            Message::assistant_text("Understood"),
        });

        std::string current_session_id;
        const auto result = app::start_new_session(loop, session_store, current_session_id, app::make_cli_session_metadata("test-model", "scope:test", "coder"));

        expect(result.had_history);
        expect(result.distillation.distilled);
        expect(loop.history().empty());
        expect(current_session_id.empty());

        const auto sessions = session_store.list_sessions("scope:test");
        expect(sessions.size() == 1_ul);
        expect(result.previous_session_id == sessions.front().id);
        expect(sessions.front().agent_key == "coder");
        expect(sessions.front().origin_kind == "cli");
        expect(sessions.front().origin_ref == "cli:local");
        const auto memory = memory_store.recall("project.current", "scope:test");
        expect(memory.find("orangutan refactor") != std::string::npos);
    };

    "load_session_into_agent_rejects_scope_mismatch"_test = [] {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools);

        const auto session_id = session_store.save({Message::user_text("hello")}, app::make_cli_session_metadata("test-model", "scope:one", "scope-one"));
        std::string current_session_id;
        const auto result = app::load_session_into_agent(session_id, loop, session_store, current_session_id, "scope:two", "scope-one");

        expect(not result.loaded);
        expect(result.status.find("does not belong") != std::string::npos);
        expect(loop.history().empty());
    };

    "resolve_requested_session_supports_latest"_test = [] {
        SessionWorkflowHarness harness;
        SessionStore session_store(harness.session_db_path());
        const auto first_id = session_store.save({Message::user_text("first")}, app::make_cli_session_metadata("test-model", "scope:test", "coder"));
        const auto second_id = session_store.save({Message::user_text("second")}, app::make_cli_session_metadata("test-model", "scope:test", "coder"));

        const auto latest = app::resolve_requested_session(session_store, "latest", "scope:test", "coder");
        expect(latest.has_value() >> fatal);
        if (latest.has_value()) {
            expect(*latest != first_id);
            expect(*latest == second_id);
        }
    };

    "resolve_requested_session_uses_agent_ownership_when_scope_is_empty"_test = [] {
        SessionWorkflowHarness harness;
        SessionStore session_store(harness.session_db_path());
        session_store.save({Message::user_text("coder")}, app::make_cli_session_metadata("test-model", "agent:coder", "coder"));
        const auto first_default = session_store.save({Message::user_text("first default")}, app::make_cli_session_metadata("test-model", "", "default"));
        const auto second_default = session_store.save({Message::user_text("second default")}, app::make_cli_session_metadata("test-model", "", "default"));

        const auto latest = app::resolve_requested_session(session_store, "latest", "", "default");
        expect(latest.has_value() >> fatal);
        if (latest.has_value()) {
            expect(*latest != first_default);
            expect(*latest == second_default);
        }
    };

    "start_new_session_writes_mirror_artifacts_when_enabled"_test = [] {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        MemoryStore memory_store(harness.memory_db_path());
        const auto workspace = orangutan::testing::unique_test_root("session-workflow-mirror");
        RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{
                                                       .scope = "scope:test",
                                                       .workspace = workspace.string(),
                                                       .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                                   });
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools, {}, &runtime_memory);

        loop.set_history({
            Message::user_text("we are working on orangutan refactor"),
            Message::assistant_text("Understood"),
        });

        std::string current_session_id;
        const auto result = app::start_new_session(loop, session_store, current_session_id, app::make_cli_session_metadata("test-model", "scope:test", "coder"));
        expect(result.distillation.distilled);

        const auto snapshot = workspace / "MEMORY.md";
        expect(std::filesystem::exists(snapshot) >> fatal);
        expect(std::ifstream(snapshot).peek() != std::ifstream::traits_type::eof());

        const auto journal_root = workspace / "memory";
        expect(std::filesystem::exists(journal_root) >> fatal);
        auto it = std::filesystem::directory_iterator(journal_root);
        expect((it != std::filesystem::directory_iterator{}) >> fatal);

        std::filesystem::remove_all(workspace);
    };

    "describe_new_session_result_uses_markdown_slash_reply_format"_test = [] {
        const auto text = app::describe_new_session_result(
            app::NewSessionResult{
                .had_history = true,
                .distillation =
                    {
                        .distilled = true,
                        .memories_stored = 3,
                    },
            },
            true);

        expect(text == "## Session\n- ✨ Started a new session.");
    };

    "export_session_markdown_writes_complete_transcript_to_workspace_exports"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("session-export");

        const auto result = app::export_session_markdown(
            {
                Message::user_text("hello"),
                {.role = Role::Assistant, .content = {ToolUseBlock{.id = "call-1", .name = "read", .input = json{{"path", "README.md"}}}}},
                {.role = Role::User, .content = {ToolResultBlock{.tool_use_id = "call-1", .content = "file contents", .is_error = false}}},
            },
            "session-123", workspace.string());

        expect(result.exported >> fatal);
        expect(std::filesystem::exists(result.path) >> fatal);

        std::ifstream in(result.path);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(content.find("# Session Export") != std::string::npos);
        expect(content.find("- Session: `session-123`") != std::string::npos);
        expect(content.find("## User 1") != std::string::npos);
        expect(content.find("hello") != std::string::npos);
        expect(content.find("### Tool Use: `read`") != std::string::npos);
        expect(content.find("\"path\": \"README.md\"") != std::string::npos);
        expect(content.find("### Tool Result") != std::string::npos);
        expect(content.find("file contents") != std::string::npos);
        expect(app::describe_export_result(result) == "## Export\n- Saved current session to `" + result.path + '`');

        std::filesystem::remove_all(workspace);
    };
};

} // namespace
