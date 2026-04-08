#include "cli/session-workflow.hpp"

#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "tools/registry/tool.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    class DistillingWorkflowProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            return {
                .stop_reason = "end_turn",
                .content = {Text{"memory|project|project.current|0.8|orangutan refactor\n"
                                 "journal|Reviewed session decisions"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
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
        SessionWorkflowHarness(const SessionWorkflowHarness &) = delete;
        SessionWorkflowHarness &operator=(const SessionWorkflowHarness &) = delete;
        SessionWorkflowHarness(SessionWorkflowHarness &&) = delete;
        SessionWorkflowHarness &operator=(SessionWorkflowHarness &&) = delete;

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

    TEST_CASE("start_new_session_distills_and_persists_previous_history") {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        MemoryStore memory_store(harness.memory_db_path());
        RuntimeMemory runtime_memory(memory_store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "scope:test"});
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools, &runtime_memory);

        loop.set_history({
            Message::user().text("we are working on orangutan refactor"),
            Message::assistant().text("Understood"),
        });

        std::string current_session_id;
        const auto result = cli::start_new_session(loop, session_store, current_session_id, cli::make_cli_session_metadata("test-model", "scope:test", "coder"));

        CHECK(result.had_history);
        CHECK(result.distillation.distilled);
        CHECK(loop.history().empty());
        CHECK(current_session_id.empty());

        const auto sessions = session_store.list_sessions("scope:test");
        CHECK(sessions.size() == 1UL);
        CHECK(result.previous_session_id == sessions.front().id);
        CHECK(sessions.front().agent_key == "coder");
        CHECK(sessions.front().origin_kind == "cli");
        CHECK(sessions.front().origin_ref == "cli:local");
        const auto memory = memory_store.recall("project.current", "scope:test");
        CHECK(memory.contains("orangutan refactor"));
    };

    TEST_CASE("load_session_into_agent_rejects_scope_mismatch") {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools);

        const auto session_id = session_store.save({Message::user().text("hello")}, cli::make_cli_session_metadata("test-model", "scope:one", "scope-one"));
        std::string current_session_id;
        const auto result = cli::load_session_into_agent(session_id, loop, session_store, current_session_id, "scope:two", "scope-one");

        CHECK_FALSE(result.loaded);
        CHECK(result.status.contains("does not belong"));
        CHECK(loop.history().empty());
    };

    TEST_CASE("resolve_requested_session_supports_latest") {
        SessionWorkflowHarness harness;
        SessionStore session_store(harness.session_db_path());
        const auto first_id = session_store.save({Message::user().text("first")}, cli::make_cli_session_metadata("test-model", "scope:test", "coder"));
        const auto second_id = session_store.save({Message::user().text("second")}, cli::make_cli_session_metadata("test-model", "scope:test", "coder"));

        const auto latest = cli::resolve_requested_session(session_store, "latest", "scope:test", "coder");
        REQUIRE(latest.has_value());
        if (latest.has_value()) {
            CHECK(*latest != first_id);
            CHECK(*latest == second_id);
        }
    };

    TEST_CASE("resolve_requested_session_uses_agent_ownership_when_scope_is_empty") {
        SessionWorkflowHarness harness;
        SessionStore session_store(harness.session_db_path());
        session_store.save({Message::user().text("coder")}, cli::make_cli_session_metadata("test-model", "agent:coder", "coder"));
        const auto first_default = session_store.save({Message::user().text("first default")}, cli::make_cli_session_metadata("test-model", "", "default"));
        const auto second_default = session_store.save({Message::user().text("second default")}, cli::make_cli_session_metadata("test-model", "", "default"));

        const auto latest = cli::resolve_requested_session(session_store, "latest", "", "default");
        REQUIRE(latest.has_value());
        if (latest.has_value()) {
            CHECK(*latest != first_default);
            CHECK(*latest == second_default);
        }
    };

    TEST_CASE("start_new_session_writes_mirror_artifacts_when_enabled") {
        SessionWorkflowHarness harness;
        DistillingWorkflowProvider provider;
        ToolRegistry tools;
        MemoryStore memory_store(harness.memory_db_path());
        const auto workspace = orangutan::testing::unique_test_root("session-workflow-mirror");
        RuntimeMemory runtime_memory(memory_store, orangutan::bootstrap::RuntimeMemoryContext{
                                                       .scope = "scope:test",
                                                       .workspace = workspace.string(),
                                                       .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                                   });
        SessionStore session_store(harness.session_db_path());
        AgentLoop loop(provider, tools, &runtime_memory);

        loop.set_history({
            Message::user().text("we are working on orangutan refactor"),
            Message::assistant().text("Understood"),
        });

        std::string current_session_id;
        const auto result = cli::start_new_session(loop, session_store, current_session_id, cli::make_cli_session_metadata("test-model", "scope:test", "coder"));
        CHECK(result.distillation.distilled);

        const auto snapshot = workspace / "MEMORY.md";
        REQUIRE(std::filesystem::exists(snapshot));
        CHECK(std::ifstream(snapshot).peek() != std::ifstream::traits_type::eof());

        const auto journal_root = workspace / "memory";
        REQUIRE(std::filesystem::exists(journal_root));
        auto it = std::filesystem::directory_iterator(journal_root);
        REQUIRE(it != std::filesystem::directory_iterator{});

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("describe_new_session_result_uses_markdown_slash_reply_format") {
        const auto text = cli::describe_new_session_result(
            cli::NewSessionResult{
                .had_history = true,
                .distillation =
                    {
                        .distilled = true,
                        .memories_stored = 3,
                    },
            },
            true);

        CHECK(text == "## Session\n- ✨ Started a new session.");
    };

    TEST_CASE("export_session_markdown_writes_complete_transcript_to_workspace_exports") {
        const auto workspace = orangutan::testing::unique_test_root("session-export");

        const auto result = cli::export_session_markdown(
            {
                Message::user().text("hello"),
                Message(base::role::assistant, {ToolUse("call-1", "read", nlohmann::json{{"path", "README.md"}})}),
                Message(base::role::user, {ToolResult("call-1", "file contents", false)}),
            },
            "session-123", workspace.string());

        REQUIRE(result.exported);
        REQUIRE(std::filesystem::exists(result.path));

        std::ifstream in(result.path);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content.contains("# Session Export"));
        CHECK(content.contains("- Session: `session-123`"));
        CHECK(content.contains("## User 1"));
        CHECK(content.contains("hello"));
        CHECK(content.contains("### Tool Use: `read`"));
        CHECK(content.contains("\"path\": \"README.md\""));
        CHECK(content.contains("### Tool Result"));
        CHECK(content.contains("file contents"));
        CHECK(cli::describe_export_result(result) == "## Export\n- Saved current session to `" + result.path + '`');

        std::filesystem::remove_all(workspace);
    };

} // namespace
