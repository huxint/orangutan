#include "app/session-workflow.hpp"

#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "core/tools/tool.hpp"

#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

SessionMetadata cli_metadata(const std::string &model, const std::string &scope_key, const std::string &agent_key) {
    return app::make_cli_session_metadata(model, scope_key, agent_key);
}

class DistillingWorkflowProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "memory|project|project.current|0.8|orangutan refactor\n"
                                          "journal|Reviewed session decisions"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used");
    }

    std::string name() const override {
        return "workflow-provider";
    }
};

class SessionWorkflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        session_db_path_ = std::filesystem::temp_directory_path() / "orangutan_session_workflow_test.db";
        memory_db_path_ = std::filesystem::temp_directory_path() / "orangutan_session_workflow_memory_test.db";
        std::filesystem::remove(session_db_path_);
        std::filesystem::remove(memory_db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(session_db_path_);
        std::filesystem::remove(memory_db_path_);
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
    std::filesystem::path session_db_path_;
    std::filesystem::path memory_db_path_;
};

} // namespace

TEST_F(SessionWorkflowTest, StartNewSessionDistillsAndPersistsPreviousHistory) {
    DistillingWorkflowProvider provider;
    ToolRegistry tools;
    MemoryStore memory_store(memory_db_path().string());
    RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{.scope = "scope:test"});
    SessionStore session_store(session_db_path().string());
    AgentLoop loop(provider, tools, {}, &runtime_memory);

    loop.set_history({
        Message::user_text("we are working on orangutan refactor"),
        Message::assistant_text("Understood"),
    });

    std::string current_session_id;
    const auto result = app::start_new_session(loop, session_store, current_session_id, cli_metadata("test-model", "scope:test", "coder"));

    EXPECT_TRUE(result.had_history);
    EXPECT_TRUE(result.distillation.distilled);
    EXPECT_EQ(loop.history().size(), 0U);
    EXPECT_TRUE(current_session_id.empty());

    const auto sessions = session_store.list_sessions("scope:test");
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(result.previous_session_id, sessions.front().id);
    EXPECT_EQ(sessions.front().agent_key, "coder");
    EXPECT_EQ(sessions.front().origin_kind, "cli");
    EXPECT_EQ(sessions.front().origin_ref, "cli:local");
    const auto memory = memory_store.recall("project.current", "scope:test");
    EXPECT_NE(memory.find("orangutan refactor"), std::string::npos);
}

TEST_F(SessionWorkflowTest, LoadSessionIntoAgentRejectsScopeMismatch) {
    DistillingWorkflowProvider provider;
    ToolRegistry tools;
    SessionStore session_store(session_db_path().string());
    AgentLoop loop(provider, tools);

    const auto session_id = session_store.save({Message::user_text("hello")}, cli_metadata("test-model", "scope:one", "scope-one"));
    std::string current_session_id;
    const auto result = app::load_session_into_agent(session_id, loop, session_store, current_session_id, "scope:two", "scope-one");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.status.find("does not belong"), std::string::npos);
    EXPECT_TRUE(loop.history().empty());
}

TEST_F(SessionWorkflowTest, ResolveRequestedSessionSupportsLatest) {
    SessionStore session_store(session_db_path().string());
    const auto first_id = session_store.save({Message::user_text("first")}, cli_metadata("test-model", "scope:test", "coder"));
    const auto second_id = session_store.save({Message::user_text("second")}, cli_metadata("test-model", "scope:test", "coder"));

    const auto latest = app::resolve_requested_session(session_store, "latest", "scope:test", "coder");
    ASSERT_TRUE(latest.has_value());
    EXPECT_NE(*latest, first_id);
    EXPECT_EQ(*latest, second_id);
}

TEST_F(SessionWorkflowTest, ResolveRequestedSessionUsesAgentOwnershipWhenScopeIsEmpty) {
    SessionStore session_store(session_db_path().string());
    session_store.save({Message::user_text("coder")}, cli_metadata("test-model", "agent:coder", "coder"));
    const auto first_default = session_store.save({Message::user_text("first default")}, cli_metadata("test-model", "", "default"));
    const auto second_default = session_store.save({Message::user_text("second default")}, cli_metadata("test-model", "", "default"));

    const auto latest = app::resolve_requested_session(session_store, "latest", "", "default");
    ASSERT_TRUE(latest.has_value());
    EXPECT_NE(*latest, first_default);
    EXPECT_EQ(*latest, second_default);
}

TEST_F(SessionWorkflowTest, StartNewSessionWritesMirrorArtifactsWhenEnabled) {
    DistillingWorkflowProvider provider;
    ToolRegistry tools;
    MemoryStore memory_store(memory_db_path().string());
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_session_workflow_mirror_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    RuntimeMemory runtime_memory(memory_store, RuntimeMemoryContext{
                                                   .scope = "scope:test",
                                                   .workspace = workspace.string(),
                                                   .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                               });
    SessionStore session_store(session_db_path().string());
    AgentLoop loop(provider, tools, {}, &runtime_memory);

    loop.set_history({
        Message::user_text("we are working on orangutan refactor"),
        Message::assistant_text("Understood"),
    });

    std::string current_session_id;
    const auto result = app::start_new_session(loop, session_store, current_session_id, cli_metadata("test-model", "scope:test", "coder"));
    EXPECT_TRUE(result.distillation.distilled);

    const auto snapshot = workspace / "MEMORY.md";
    ASSERT_TRUE(std::filesystem::exists(snapshot));
    EXPECT_NE(std::ifstream(snapshot).peek(), std::ifstream::traits_type::eof());

    const auto journal_root = workspace / "memory";
    ASSERT_TRUE(std::filesystem::exists(journal_root));
    auto it = std::filesystem::directory_iterator(journal_root);
    ASSERT_NE(it, std::filesystem::directory_iterator{});

    std::filesystem::remove_all(workspace);
}

TEST_F(SessionWorkflowTest, DescribeNewSessionResultUsesMarkdownSlashReplyFormat) {
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

    EXPECT_EQ(text, "## Session\n"
                    "- ✨ Started a new session.");
}

TEST_F(SessionWorkflowTest, ExportSessionMarkdownWritesCompleteTranscriptToWorkspaceExports) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_session_export_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    const auto result = app::export_session_markdown(
        {
            Message::user_text("hello"),
            {.role = "assistant", .content = {ToolUseBlock{.id = "call-1", .name = "read", .input = json{{"path", "README.md"}}}}},
            {.role = "user", .content = {ToolResultBlock{.tool_use_id = "call-1", .content = "file contents", .is_error = false}}},
        },
        "session-123", workspace.string());

    ASSERT_TRUE(result.exported);
    ASSERT_TRUE(std::filesystem::exists(result.path));

    std::ifstream in(result.path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("# Session Export"), std::string::npos);
    EXPECT_NE(content.find("- Session: `session-123`"), std::string::npos);
    EXPECT_NE(content.find("## User 1"), std::string::npos);
    EXPECT_NE(content.find("hello"), std::string::npos);
    EXPECT_NE(content.find("### Tool Use: `read`"), std::string::npos);
    EXPECT_NE(content.find("\"path\": \"README.md\""), std::string::npos);
    EXPECT_NE(content.find("### Tool Result"), std::string::npos);
    EXPECT_NE(content.find("file contents"), std::string::npos);
    EXPECT_EQ(app::describe_export_result(result), "## Export\n- Saved current session to `" + result.path + '`');

    std::filesystem::remove_all(workspace);
}
