#include "infra/storage/session-store.hpp"
#include "core/types.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>

namespace {

void exec_sql(sqlite3 *db, const char *sql) {
    char *err_msg = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err_msg), SQLITE_OK) << (err_msg != nullptr ? err_msg : "sqlite error");
    sqlite3_free(err_msg);
}

} // namespace

using namespace orangutan;

class SessionStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_test.db";
        // Remove any leftover test database
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    [[nodiscard]]
    const std::filesystem::path &db_path() const {
        return db_path_;
    }

private:
    std::filesystem::path db_path_;
};

TEST_F(SessionStoreTest, SaveAndLoadTextMessages) {
    SessionStore store(db_path().string());

    std::vector<Message> messages = {
        Message::user_text("Hello"),
        Message::assistant_text("Hi there!"),
    };

    auto session_id = store.save(messages, "claude-sonnet-4-20250514");

    auto loaded = store.load(session_id);

    ASSERT_EQ(loaded.size(), 2);
    EXPECT_EQ(loaded[0].role, "user");
    EXPECT_EQ(loaded[1].role, "assistant");

    const auto *user_text = std::get_if<TextBlock>(&loaded[0].content[0]);
    ASSERT_NE(user_text, nullptr);
    EXPECT_EQ(user_text->text, "Hello");

    const auto *asst_text = std::get_if<TextBlock>(&loaded[1].content[0]);
    ASSERT_NE(asst_text, nullptr);
    EXPECT_EQ(asst_text->text, "Hi there!");
}

TEST_F(SessionStoreTest, SaveAndLoadToolUseBlocks) {
    SessionStore store(db_path().string());

    std::vector<ContentBlock> content;
    content.emplace_back(ToolUseBlock{
        .id = "tool_123",
        .name = "shell",
        .input = json{{"command", "ls -la"}},
    });

    std::vector<Message> messages = {
        Message::user_text("list files"),
        {.role = "assistant", .content = std::move(content)},
    };

    auto session_id = store.save(messages, "test-model");
    auto loaded = store.load(session_id);

    ASSERT_EQ(loaded.size(), 2);
    const auto *tool = std::get_if<ToolUseBlock>(&loaded[1].content[0]);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->id, "tool_123");
    EXPECT_EQ(tool->name, "shell");
    EXPECT_EQ(tool->input["command"], "ls -la");
}

TEST_F(SessionStoreTest, SaveAndLoadToolResultBlocks) {
    SessionStore store(db_path().string());

    std::vector<ContentBlock> result_content;
    result_content.emplace_back(ToolResultBlock{
        .tool_use_id = "tool_123",
        .content = "file1.txt\nfile2.cpp",
        .is_error = false,
    });

    std::vector<Message> messages = {
        {.role = "user", .content = std::move(result_content)},
    };

    auto session_id = store.save(messages, "test-model");
    auto loaded = store.load(session_id);

    ASSERT_EQ(loaded.size(), 1);
    const auto *result = std::get_if<ToolResultBlock>(&loaded[0].content[0]);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->tool_use_id, "tool_123");
    EXPECT_EQ(result->content, "file1.txt\nfile2.cpp");
    EXPECT_FALSE(result->is_error);
}

TEST_F(SessionStoreTest, ToolResultPreservesErrorFlag) {
    SessionStore store(db_path().string());

    std::vector<ContentBlock> content;
    content.emplace_back(ToolResultBlock{
        .tool_use_id = "err_1",
        .content = "command not found",
        .is_error = true,
    });

    std::vector<Message> messages = {{.role = "user", .content = std::move(content)}};

    auto session_id = store.save(messages, "test-model");
    auto loaded = store.load(session_id);

    const auto *result = std::get_if<ToolResultBlock>(&loaded[0].content[0]);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->is_error);
}

TEST_F(SessionStoreTest, ListSessionsReturnsSavedSessions) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    store.save(messages, "model-a", "scope:a");
    store.save(messages, "model-b", "scope:b");

    auto sessions = store.list_sessions();
    ASSERT_EQ(sessions.size(), 2);

    // Both sessions present with correct message counts
    std::set<std::string> models;
    for (const auto &s : sessions) {
        models.insert(s.model);
        EXPECT_EQ(s.message_count, 1);
    }
    EXPECT_TRUE(models.count("model-a"));
    EXPECT_TRUE(models.count("model-b"));
}

TEST_F(SessionStoreTest, ListSessionsCanFilterByScope) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto scope_a = store.save(messages, "model-a", "scope:a");
    store.save(messages, "model-b", "scope:b");

    auto sessions = store.list_sessions("scope:a");
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].id, scope_a);
    EXPECT_EQ(sessions[0].scope_key, "scope:a");
}

TEST_F(SessionStoreTest, SessionBelongsToScopeChecksOwnership) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto session_id = store.save(messages, "model-a", "scope:a");

    EXPECT_TRUE(store.session_belongs_to_scope(session_id, "scope:a"));
    EXPECT_FALSE(store.session_belongs_to_scope(session_id, "scope:b"));
}

TEST_F(SessionStoreTest, SavePersistsExplicitSessionMetadata) {
    SessionStore store(db_path().string());

    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };
    const std::vector<Message> messages = {Message::user_text("Hello")};

    const auto session_id = store.save(messages, metadata);

    const auto sessions = store.list_sessions();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].id, session_id);
    EXPECT_EQ(sessions[0].model, metadata.model);
    EXPECT_EQ(sessions[0].scope_key, metadata.scope_key);
    EXPECT_EQ(sessions[0].agent_key, metadata.agent_key);
    EXPECT_EQ(sessions[0].origin_kind, metadata.origin_kind);
    EXPECT_EQ(sessions[0].origin_ref, metadata.origin_ref);
}

TEST_F(SessionStoreTest, CreateEmptyPersistsExplicitSessionMetadata) {
    SessionStore store(db_path().string());

    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };

    const auto session_id = store.create_empty(metadata);

    const auto sessions = store.list_sessions();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].id, session_id);
    EXPECT_EQ(sessions[0].message_count, 0);
    EXPECT_EQ(sessions[0].agent_key, metadata.agent_key);
    EXPECT_EQ(sessions[0].origin_kind, metadata.origin_kind);
    EXPECT_EQ(sessions[0].origin_ref, metadata.origin_ref);
}

TEST_F(SessionStoreTest, ListSessionsForAgentReturnsOnlyMatchingSessions) {
    SessionStore store(db_path().string());

    const std::vector<Message> messages = {Message::user_text("Hello")};
    const SessionMetadata coder_metadata{
        .model = "test-model-coder",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };
    const SessionMetadata default_metadata{
        .model = "test-model-default",
        .scope_key = "agent:default",
        .agent_key = "default",
        .origin_kind = "web",
        .origin_ref = "web:local",
    };

    const auto coder_session_id = store.save(messages, coder_metadata);
    store.save(messages, default_metadata);

    const auto coder_sessions = store.list_sessions_for_agent("coder");
    ASSERT_EQ(coder_sessions.size(), 1U);
    EXPECT_EQ(coder_sessions[0].id, coder_session_id);
    EXPECT_EQ(coder_sessions[0].agent_key, "coder");
}

TEST_F(SessionStoreTest, SessionBelongsToAgentChecksOwnership) {
    SessionStore store(db_path().string());

    const std::vector<Message> messages = {Message::user_text("Hello")};
    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };
    const auto session_id = store.save(messages, metadata);

    EXPECT_TRUE(store.session_belongs_to_agent(session_id, "coder"));
    EXPECT_FALSE(store.session_belongs_to_agent(session_id, "default"));
}

TEST_F(SessionStoreTest, RemoveDeletesSession) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("test")};
    auto id = store.save(messages, "test-model");

    EXPECT_EQ(store.list_sessions().size(), 1);

    store.remove(id);

    EXPECT_EQ(store.list_sessions().size(), 0);
    EXPECT_THROW(store.load(id), std::runtime_error);
}

TEST_F(SessionStoreTest, LoadNonexistentSessionThrows) {
    SessionStore store(db_path().string());
    EXPECT_THROW(store.load("nonexistent-id"), std::runtime_error);
}

TEST_F(SessionStoreTest, CreateEmptySessionLoadsEmptyHistory) {
    SessionStore store(db_path().string());

    const auto session_id = store.create_empty("claude-sonnet-4-20250514", "scope:a");
    EXPECT_FALSE(session_id.empty());

    const auto loaded = store.load(session_id);
    EXPECT_TRUE(loaded.empty());

    const auto sessions = store.list_sessions("scope:a");
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].id, session_id);
    EXPECT_EQ(sessions[0].message_count, 0);
}

TEST_F(SessionStoreTest, AppendPopulatesPreviouslyEmptySession) {
    SessionStore store(db_path().string());

    const auto session_id = store.create_empty("claude-sonnet-4-20250514");
    const std::vector<Message> messages = {
        Message::user_text("Hello"),
        Message::assistant_text("Hi there!"),
    };

    store.append(session_id, messages, 0);

    const auto loaded = store.load(session_id);
    ASSERT_EQ(loaded.size(), 2);
    const auto *assistant_text = std::get_if<TextBlock>(&loaded[1].content[0]);
    ASSERT_NE(assistant_text, nullptr);
    EXPECT_EQ(assistant_text->text, "Hi there!");
}

TEST_F(SessionStoreTest, MultipleContentBlocksInOneMessage) {
    SessionStore store(db_path().string());

    std::vector<ContentBlock> content;
    content.emplace_back(TextBlock{.text = "I'll run that command"});
    content.emplace_back(ToolUseBlock{
        .id = "call_1",
        .name = "shell",
        .input = json{{"command", "echo hi"}},
    });

    std::vector<Message> messages = {{.role = "assistant", .content = std::move(content)}};

    auto id = store.save(messages, "test-model");
    auto loaded = store.load(id);

    ASSERT_EQ(loaded[0].content.size(), 2);
    EXPECT_NE(std::get_if<TextBlock>(&loaded[0].content[0]), nullptr);
    EXPECT_NE(std::get_if<ToolUseBlock>(&loaded[0].content[1]), nullptr);
}

TEST_F(SessionStoreTest, LatestSessionIdEmptyDb) {
    SessionStore store(db_path().string());
    EXPECT_EQ(store.latest_session_id(), std::nullopt);
}

TEST_F(SessionStoreTest, LatestSessionIdReturnsNewest) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    store.save(messages, "model-a");
    auto second_id = store.save(messages, "model-b");

    auto latest = store.latest_session_id();
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(*latest, second_id);
}

TEST_F(SessionStoreTest, CanBindAndResolveSessionForJid) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto session_id = store.save(messages, "model-a");

    EXPECT_EQ(store.bound_session_for_jid("qqbot:c2c:alice"), std::nullopt);

    store.bind_jid("qqbot:c2c:alice", session_id);

    auto bound = store.bound_session_for_jid("qqbot:c2c:alice");
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, session_id);
}

TEST_F(SessionStoreTest, JidBindingsAreScopedByAgentKey) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto general_session_id = store.save(messages, "model-a", "agent:default|jid:qqbot:c2c:alice");
    auto coder_session_id = store.save(messages, "model-b", "agent:coder|jid:qqbot:c2c:alice");

    store.bind_jid("qqbot:c2c:alice", general_session_id, "default");
    store.bind_jid("qqbot:c2c:alice", coder_session_id, "coder");

    auto general_bound = store.bound_session_for_jid("qqbot:c2c:alice", "default");
    auto coder_bound = store.bound_session_for_jid("qqbot:c2c:alice", "coder");

    ASSERT_TRUE(general_bound.has_value());
    ASSERT_TRUE(coder_bound.has_value());
    EXPECT_EQ(*general_bound, general_session_id);
    EXPECT_EQ(*coder_bound, coder_session_id);
}

TEST_F(SessionStoreTest, CanClearJidBinding) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto session_id = store.save(messages, "model-a");
    store.bind_jid("qqbot:c2c:alice", session_id);

    store.clear_jid("qqbot:c2c:alice");

    EXPECT_EQ(store.bound_session_for_jid("qqbot:c2c:alice"), std::nullopt);
}

TEST_F(SessionStoreTest, RemovingSessionClearsJidBinding) {
    SessionStore store(db_path().string());

    auto messages = std::vector{Message::user_text("Hello")};
    auto session_id = store.save(messages, "model-a");
    store.bind_jid("qqbot:c2c:alice", session_id);

    store.remove(session_id);

    EXPECT_EQ(store.bound_session_for_jid("qqbot:c2c:alice"), std::nullopt);
}

TEST_F(SessionStoreTest, AppendAddsOnlyNewMessages) {
    SessionStore store(db_path().string());

    std::vector<Message> messages = {
        Message::user_text("Hello"),
        Message::assistant_text("Hi there!"),
    };

    auto session_id = store.save(messages, "model-a");
    messages.push_back(Message::user_text("How are you?"));
    messages.push_back(Message::assistant_text("Doing well."));

    store.append(session_id, messages, 2);

    auto loaded = store.load(session_id);
    ASSERT_EQ(loaded.size(), 4);
    const auto *last_text = std::get_if<TextBlock>(&loaded[3].content[0]);
    ASSERT_NE(last_text, nullptr);
    EXPECT_EQ(last_text->text, "Doing well.");
}

TEST_F(SessionStoreTest, UpdateAndAppendCanRefreshStoredModelMetadata) {
    SessionStore store(db_path().string());

    std::vector<Message> messages = {
        Message::user_text("Hello"),
        Message::assistant_text("Hi there!"),
    };

    const auto session_id = store.save(messages, "model-a", "scope:test");
    store.update(session_id, messages, "model-b");

    auto sessions = store.list_sessions("scope:test");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].model, "model-b");

    messages.push_back(Message::user_text("How are you?"));
    store.append(session_id, messages, 2, "model-c");

    sessions = store.list_sessions("scope:test");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].model, "model-c");
}

TEST_F(SessionStoreTest, MigratesLegacySchemaForScopeAndCompositeBindingKey) {
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &db), SQLITE_OK);

    exec_sql(db, "CREATE TABLE sessions ("
                 "id TEXT PRIMARY KEY,"
                 "model TEXT NOT NULL,"
                 "created_at TEXT NOT NULL DEFAULT (datetime('now'))"
                 ");"
                 "CREATE TABLE messages ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "session_id TEXT NOT NULL,"
                 "seq INTEGER NOT NULL,"
                 "role TEXT NOT NULL,"
                 "content_json TEXT NOT NULL"
                 ");"
                 "CREATE TABLE channel_session_bindings ("
                 "jid TEXT PRIMARY KEY,"
                 "session_id TEXT NOT NULL,"
                 "updated_at TEXT NOT NULL DEFAULT (datetime('now'))"
                 ");");

    exec_sql(db, "INSERT INTO sessions (id, model) VALUES ('legacy-session', 'legacy-model');");
    exec_sql(db, "INSERT INTO messages (session_id, seq, role, content_json) VALUES ("
                 "'legacy-session', 0, 'user', '[{"
                 "type"
                 ":"
                 "text"
                 ","
                 "text"
                 ":"
                 "hello"
                 "}]'"
                 ");");
    exec_sql(db, "INSERT INTO channel_session_bindings (jid, session_id) VALUES ('qqbot:c2c:alice', 'legacy-session');");
    sqlite3_close(db);

    SessionStore store(db_path().string());

    const auto sessions = store.list_sessions();
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0].id, "legacy-session");
    EXPECT_TRUE(sessions[0].scope_key.empty());

    sqlite3 *verify_db = nullptr;
    ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &verify_db), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(verify_db, "PRAGMA table_info(channel_session_bindings)", -1, &stmt, nullptr), SQLITE_OK);

    bool has_agent_key = false;
    int jid_pk_position = 0;
    int agent_key_pk_position = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto *text = sqlite3_column_text(stmt, 1);
        // sqlite3_column_text() returns UTF-8 bytes as unsigned char*; convert for std::string construction.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *name = text != nullptr ? reinterpret_cast<const char *>(text) : nullptr;
        const auto pk = sqlite3_column_int(stmt, 5);
        const auto column_name = name != nullptr ? std::string(name) : std::string{};
        if (column_name == "jid") {
            jid_pk_position = pk;
        }
        if (column_name == "agent_key") {
            has_agent_key = true;
            agent_key_pk_position = pk;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(verify_db);

    EXPECT_TRUE(has_agent_key);
    EXPECT_EQ(jid_pk_position, 1);
    EXPECT_EQ(agent_key_pk_position, 2);

    const auto migrated_binding = store.bound_session_for_jid("qqbot:c2c:alice");
    ASSERT_TRUE(migrated_binding.has_value());
    EXPECT_EQ(*migrated_binding, "legacy-session");
}
