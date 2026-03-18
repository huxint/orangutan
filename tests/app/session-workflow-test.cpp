#include "app/session-workflow.hpp"

#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "core/tools/tool.hpp"

#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

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
    const auto result = app::start_new_session(loop, session_store, "test-model", current_session_id, "scope:test");

    EXPECT_TRUE(result.had_history);
    EXPECT_TRUE(result.distillation.distilled);
    EXPECT_EQ(loop.history().size(), 0U);
    EXPECT_TRUE(current_session_id.empty());

    const auto sessions = session_store.list_sessions("scope:test");
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(result.previous_session_id, sessions.front().id);
    const auto memory = memory_store.recall("project.current", "scope:test");
    EXPECT_NE(memory.find("orangutan refactor"), std::string::npos);
}

TEST_F(SessionWorkflowTest, LoadSessionIntoAgentRejectsScopeMismatch) {
    DistillingWorkflowProvider provider;
    ToolRegistry tools;
    SessionStore session_store(session_db_path().string());
    AgentLoop loop(provider, tools);

    const auto session_id = session_store.save({Message::user_text("hello")}, "test-model", "scope:one");
    std::string current_session_id;
    const auto result = app::load_session_into_agent(session_id, loop, session_store, current_session_id, "scope:two");

    EXPECT_FALSE(result.loaded);
    EXPECT_NE(result.status.find("does not belong"), std::string::npos);
    EXPECT_TRUE(loop.history().empty());
}

TEST_F(SessionWorkflowTest, ResolveRequestedSessionSupportsLatest) {
    SessionStore session_store(session_db_path().string());
    const auto first_id = session_store.save({Message::user_text("first")}, "test-model", "scope:test");
    const auto second_id = session_store.save({Message::user_text("second")}, "test-model", "scope:test");

    const auto latest = app::resolve_requested_session(session_store, "latest", "scope:test");
    ASSERT_TRUE(latest.has_value());
    EXPECT_NE(*latest, first_id);
    EXPECT_EQ(*latest, second_id);
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
    const auto result = app::start_new_session(loop, session_store, "test-model", current_session_id, "scope:test");
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
