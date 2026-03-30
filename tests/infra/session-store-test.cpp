#include "infra/storage/session-store.hpp"
#include "core/types.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

namespace {

    struct SessionStoreHarness {
        SessionStoreHarness()
        : db_path(orangutan::testing::unique_test_db_path("session-store", "sessions.db")) {}

        ~SessionStoreHarness() {
            std::filesystem::remove_all(db_path.parent_path());
        }

        [[nodiscard]]
        orangutan::SessionStore store() const {
            return orangutan::SessionStore(db_path);
        }

        std::filesystem::path db_path;
    };

    struct SqliteDb {
        explicit SqliteDb(const std::filesystem::path &path) {
            const auto rc = sqlite3_open(path.string().c_str(), &db);
            INFO("failed to open sqlite database");
            CHECK((rc == SQLITE_OK));
        }

        ~SqliteDb() {
            if (db != nullptr) {
                sqlite3_close(db);
            }
        }

        sqlite3 *db = nullptr;
    };

    void exec_sql(sqlite3 *db, const char *sql) {
        char *err_msg = nullptr;
        const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
        INFO((err_msg != nullptr ? err_msg : "sqlite error"));
        CHECK((rc == SQLITE_OK));
        sqlite3_free(err_msg);
    }

    orangutan::SessionMetadata make_session_metadata(std::string model, std::string scope_key = {}) {
        return orangutan::SessionMetadata{
            .model = model,
            .scope_key = scope_key,
            .agent_key = "",
            .origin_kind = "cli",
            .origin_ref = "",
        };
    }

} // namespace

using namespace orangutan;
using namespace orangutan::testing;

TEST_CASE("save_and_load_text_messages") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Message> messages = {
        Message::user().text("Hello"),
        Message::assistant().text("Hi there!"),
    };

    const auto session_id = store.save(messages, make_session_metadata("claude-sonnet-4-20250514"));
    const auto loaded = store.load(session_id);

    INFO("expected two persisted messages");
    REQUIRE(loaded.size() == std::std::size_t{2});
    CHECK(loaded[0].role() == base::role::user);
    CHECK(loaded[1].role() == base::role::assistant);

    const auto *user_text = std::get_if<Text>(&*loaded[0].begin());
    REQUIRE(user_text != nullptr);
    if (user_text != nullptr) {
        CHECK(user_text->text == "Hello");
    }

    const auto *asst_text = std::get_if<Text>(&*loaded[1].begin());
    REQUIRE(asst_text != nullptr);
    if (asst_text != nullptr) {
        CHECK(asst_text->text == "Hi there!");
    }
};

TEST_CASE("save_and_load_tool_use_blocks") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Content> content;
    content.emplace_back(ToolUse{
        .id = "tool_123",
        .name = "shell",
        .input = nlohmann::json{{"command", "ls -la"}},
    });

    std::vector<Message> messages = {
        Message::user().text("list files"),
        Message(base::role::assistant, std::move(content)),
    };

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    INFO("expected tool-use history roundtrip");
    REQUIRE(loaded.size() == std::std::size_t{2});
    const auto *tool = std::get_if<ToolUse>(&*loaded[1].begin());
    REQUIRE(tool != nullptr);
    if (tool != nullptr) {
        CHECK(tool->id == "tool_123");
        CHECK(tool->name == "shell");
        CHECK(tool->input["command"] == "ls -la");
    }
};

TEST_CASE("save_and_load_tool_result_blocks") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Content> result_content;
    result_content.emplace_back(ToolResult{
        .tool_use_id = "tool_123",
        .content = "file1.txt\nfile2.cpp",
        .is_error = false,
    });

    std::vector<Message> messages = {
        Message(base::role::user, std::move(result_content)),
    };

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    INFO("expected one persisted tool result");
    REQUIRE(loaded.size() == std::std::size_t{1});
    const auto *result = std::get_if<ToolResult>(&*loaded[0].begin());
    REQUIRE(result != nullptr);
    if (result != nullptr) {
        CHECK(result->tool_use_id == "tool_123");
        CHECK(result->content == "file1.txt\nfile2.cpp");
        CHECK_FALSE(result->is_error);
    }
};

TEST_CASE("tool_result_preserves_error_flag") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Content> content;
    content.emplace_back(ToolResult{
        .tool_use_id = "err_1",
        .content = "command not found",
        .is_error = true,
    });

    std::vector<Message> messages = {Message(base::role::user, std::move(content))};

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    REQUIRE(loaded.size() == std::std::size_t{1});
    const auto *result = std::get_if<ToolResult>(&*loaded[0].begin());
    REQUIRE(result != nullptr);
    if (result != nullptr) {
        CHECK(result->is_error);
    }
};

TEST_CASE("list_sessions_returns_saved_sessions") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    store.save(messages, make_session_metadata("model-a", "scope:a"));
    store.save(messages, make_session_metadata("model-b", "scope:b"));

    const auto sessions = store.list_sessions();
    CHECK(sessions.size() == std::std::size_t{2});

    std::set<std::string> models;
    for (const auto &session : sessions) {
        models.insert(session.model);
        CHECK(session.message_count == 1);
    }
    CHECK(models.contains("model-a"));
    CHECK(models.contains("model-b"));
};

TEST_CASE("list_sessions_can_filter_by_scope") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto scope_a = store.save(messages, make_session_metadata("model-a", "scope:a"));
    store.save(messages, make_session_metadata("model-b", "scope:b"));

    const auto sessions = store.list_sessions("scope:a");
    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].id == scope_a);
    CHECK(sessions[0].scope_key == "scope:a");
};

TEST_CASE("session_belongs_to_scope_checks_ownership") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a", "scope:a"));

    CHECK(store.session_belongs_to_scope(session_id, "scope:a"));
    CHECK_FALSE(store.session_belongs_to_scope(session_id, "scope:b"));
};

TEST_CASE("save_persists_explicit_session_metadata") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };
    const std::vector<Message> messages = {Message::user().text("Hello")};

    const auto session_id = store.save(messages, metadata);
    const auto sessions = store.list_sessions();

    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].id == session_id);
    CHECK(sessions[0].model == metadata.model);
    CHECK(sessions[0].scope_key == metadata.scope_key);
    CHECK(sessions[0].agent_key == metadata.agent_key);
    CHECK(sessions[0].origin_kind == metadata.origin_kind);
    CHECK(sessions[0].origin_ref == metadata.origin_ref);
};

TEST_CASE("create_empty_persists_explicit_session_metadata") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };

    const auto session_id = store.create_empty(metadata);
    const auto sessions = store.list_sessions();

    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].id == session_id);
    CHECK(sessions[0].message_count == 0);
    CHECK(sessions[0].agent_key == metadata.agent_key);
    CHECK(sessions[0].origin_kind == metadata.origin_kind);
    CHECK(sessions[0].origin_ref == metadata.origin_ref);
};

TEST_CASE("list_sessions_for_agent_returns_only_matching_sessions") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const std::vector<Message> messages = {Message::user().text("Hello")};
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
    REQUIRE(coder_sessions.size() == std::std::size_t{1});
    CHECK(coder_sessions[0].id == coder_session_id);
    CHECK(coder_sessions[0].agent_key == "coder");
};

TEST_CASE("session_belongs_to_agent_checks_ownership") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const std::vector<Message> messages = {Message::user().text("Hello")};
    const SessionMetadata metadata{
        .model = "test-model",
        .scope_key = "agent:coder",
        .agent_key = "coder",
        .origin_kind = "channel",
        .origin_ref = "qqbot:c2c:alice",
    };
    const auto session_id = store.save(messages, metadata);

    CHECK(store.session_belongs_to_agent(session_id, "coder"));
    CHECK_FALSE(store.session_belongs_to_agent(session_id, "default"));
};

TEST_CASE("remove_deletes_session") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("test")};
    const auto id = store.save(messages, make_session_metadata("test-model"));

    CHECK(store.list_sessions().size() == std::std::size_t{1});
    store.remove(id);
    CHECK(store.list_sessions().empty());
    REQUIRE_THROWS_AS(store.load(id), std::runtime_error);
};

TEST_CASE("load_nonexistent_session_throws") {
    SessionStoreHarness harness;
    auto store = harness.store();

    REQUIRE_THROWS_AS(store.load("nonexistent-id"), std::runtime_error);
};

TEST_CASE("create_empty_session_loads_empty_history") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const auto session_id = store.create_empty(make_session_metadata("claude-sonnet-4-20250514", "scope:a"));
    CHECK_FALSE(session_id.empty());

    const auto loaded = store.load(session_id);
    CHECK(loaded.empty());

    const auto sessions = store.list_sessions("scope:a");
    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].id == session_id);
    CHECK(sessions[0].message_count == 0);
};

TEST_CASE("append_populates_previously_empty_session") {
    SessionStoreHarness harness;
    auto store = harness.store();

    const auto session_id = store.create_empty(make_session_metadata("claude-sonnet-4-20250514"));
    const std::vector<Message> messages = {
        Message::user().text("Hello"),
        Message::assistant().text("Hi there!"),
    };

    store.append(session_id, messages, 0);

    const auto loaded = store.load(session_id);
    REQUIRE(loaded.size() == std::std::size_t{2});
    const auto *assistant_text = std::get_if<Text>(&*loaded[1].begin());
    REQUIRE(assistant_text != nullptr);
    if (assistant_text != nullptr) {
        CHECK(assistant_text->text == "Hi there!");
    }
};

TEST_CASE("multiple_content_blocks_in_one_message") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Content> content;
    content.emplace_back(Text{"I'll run that command"});
    content.emplace_back(ToolUse{
        .id = "call_1",
        .name = "shell",
        .input = nlohmann::json{{"command", "echo hi"}},
    });

    std::vector<Message> messages = {Message(base::role::assistant, std::move(content))};

    const auto id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(id);

    REQUIRE(loaded.size() == std::std::size_t{1});
    auto first = loaded[0].begin();
    auto second = first;
    ++second;
    REQUIRE(second != loaded[0].end());
    CHECK(std::get_if<Text>(&*first) != nullptr);
    CHECK(std::get_if<ToolUse>(&*second) != nullptr);
};

TEST_CASE("latest_session_id_empty_db") {
    SessionStoreHarness harness;
    auto store = harness.store();

    CHECK(store.latest_session_id() == std::nullopt);
};

TEST_CASE("latest_session_id_returns_newest") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    store.save(messages, make_session_metadata("model-a"));
    const auto second_id = store.save(messages, make_session_metadata("model-b"));

    const auto latest = store.latest_session_id();
    REQUIRE(latest.has_value());
    if (latest.has_value()) {
        CHECK(*latest == second_id);
    }
};

TEST_CASE("can_bind_and_resolve_session_for_jid") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));

    CHECK(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
    store.bind_jid("qqbot:c2c:alice", session_id);

    const auto bound = store.bound_session_for_jid("qqbot:c2c:alice");
    REQUIRE(bound.has_value());
    if (bound.has_value()) {
        CHECK(*bound == session_id);
    }
};

TEST_CASE("jid_bindings_are_scoped_by_agent_key") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto general_session_id = store.save(messages, make_session_metadata("model-a", "agent:default|jid:qqbot:c2c:alice"));
    const auto coder_session_id = store.save(messages, make_session_metadata("model-b", "agent:coder|jid:qqbot:c2c:alice"));

    store.bind_jid("qqbot:c2c:alice", general_session_id, "default");
    store.bind_jid("qqbot:c2c:alice", coder_session_id, "coder");

    const auto general_bound = store.bound_session_for_jid("qqbot:c2c:alice", "default");
    const auto coder_bound = store.bound_session_for_jid("qqbot:c2c:alice", "coder");

    REQUIRE(general_bound.has_value());
    REQUIRE(coder_bound.has_value());
    if (general_bound.has_value()) {
        CHECK(*general_bound == general_session_id);
    }
    if (coder_bound.has_value()) {
        CHECK(*coder_bound == coder_session_id);
    }
};

TEST_CASE("can_clear_jid_binding") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));
    store.bind_jid("qqbot:c2c:alice", session_id);

    store.clear_jid("qqbot:c2c:alice");
    CHECK(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
};

TEST_CASE("removing_session_clears_jid_binding") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));
    store.bind_jid("qqbot:c2c:alice", session_id);

    store.remove(session_id);
    CHECK(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
};

TEST_CASE("append_adds_only_new_messages") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Message> messages = {
        Message::user().text("Hello"),
        Message::assistant().text("Hi there!"),
    };

    const auto session_id = store.save(messages, make_session_metadata("model-a"));
    messages.push_back(Message::user().text("How are you?"));
    messages.push_back(Message::assistant().text("Doing well."));

    store.append(session_id, messages, 2);

    const auto loaded = store.load(session_id);
    REQUIRE(loaded.size() == std::std::size_t{4});
    const auto *last_text = std::get_if<Text>(&*loaded[3].begin());
    REQUIRE(last_text != nullptr);
    if (last_text != nullptr) {
        CHECK(last_text->text == "Doing well.");
    }
};

TEST_CASE("update_and_append_can_refresh_stored_model_metadata") {
    SessionStoreHarness harness;
    auto store = harness.store();

    std::vector<Message> messages = {
        Message::user().text("Hello"),
        Message::assistant().text("Hi there!"),
    };

    const auto session_id = store.save(messages, make_session_metadata("model-a", "scope:test"));
    store.update(session_id, messages, "model-b");

    auto sessions = store.list_sessions("scope:test");
    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].model == "model-b");

    messages.push_back(Message::user().text("How are you?"));
    store.append(session_id, messages, 2, "model-c");

    sessions = store.list_sessions("scope:test");
    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].model == "model-c");
};

TEST_CASE("migrates_legacy_schema_for_scope_and_composite_binding_key") {
    SessionStoreHarness harness;

    {
        SqliteDb db(harness.db_path);
        exec_sql(db.db, "CREATE TABLE sessions ("
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
        exec_sql(db.db, "INSERT INTO sessions (id, model) VALUES ('legacy-session', 'legacy-model');");
        exec_sql(db.db, "INSERT INTO messages (session_id, seq, role, content_json) VALUES ("
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
        exec_sql(db.db, "INSERT INTO channel_session_bindings (jid, session_id) VALUES ('qqbot:c2c:alice', 'legacy-session');");
    }

    auto store = harness.store();

    const auto sessions = store.list_sessions();
    REQUIRE(sessions.size() == std::std::size_t{1});
    CHECK(sessions[0].id == "legacy-session");
    CHECK(sessions[0].scope_key.empty());

    SqliteDb verify_db(harness.db_path);
    sqlite3_stmt *stmt = nullptr;
    const auto prepare_rc = sqlite3_prepare_v2(verify_db.db, "PRAGMA table_info(channel_session_bindings)", -1, &stmt, nullptr);
    INFO("failed to inspect channel_session_bindings schema");
    REQUIRE(prepare_rc == SQLITE_OK);

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

    CHECK(has_agent_key);
    CHECK(jid_pk_position == 1);
    CHECK(agent_key_pk_position == 2);

    const auto migrated_binding = store.bound_session_for_jid("qqbot:c2c:alice");
    REQUIRE(migrated_binding.has_value());
    if (migrated_binding.has_value()) {
        CHECK(*migrated_binding == "legacy-session");
    }
};
