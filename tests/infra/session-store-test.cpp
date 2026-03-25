#include "infra/storage/session-store.hpp"
#include "core/types.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <set>
#include "support/ut.hpp"
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
        boost::ut::expect((rc == SQLITE_OK) >> boost::ut::fatal) << "failed to open sqlite database";
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
    boost::ut::expect((rc == SQLITE_OK) >> boost::ut::fatal) << (err_msg != nullptr ? err_msg : "sqlite error");
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

boost::ut::suite session_store_suite = [] {
    using namespace boost::ut;

    "save_and_load_text_messages"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<Message> messages = {
            Message::user_text("Hello"),
            Message::assistant_text("Hi there!"),
        };

        const auto session_id = store.save(messages, make_session_metadata("claude-sonnet-4-20250514"));
        const auto loaded = store.load(session_id);

        expect((loaded.size() == std::size_t{2}) >> fatal) << "expected two persisted messages";
        expect(loaded[0].role == Role::user);
        expect(loaded[1].role == Role::assistant);

        const auto *user_text = std::get_if<TextBlock>(&loaded[0].content[0]);
        expect((user_text != nullptr) >> fatal);
        if (user_text != nullptr) {
            expect(user_text->text == "Hello");
        }

        const auto *asst_text = std::get_if<TextBlock>(&loaded[1].content[0]);
        expect((asst_text != nullptr) >> fatal);
        if (asst_text != nullptr) {
            expect(asst_text->text == "Hi there!");
        }
    };

    "save_and_load_tool_use_blocks"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<ContentBlock> content;
        content.emplace_back(ToolUseBlock{
            .id = "tool_123",
            .name = "shell",
            .input = json{{"command", "ls -la"}},
        });

        std::vector<Message> messages = {
            Message::user_text("list files"),
            {.role = Role::assistant, .content = std::move(content)},
        };

        const auto session_id = store.save(messages, make_session_metadata("test-model"));
        const auto loaded = store.load(session_id);

        expect((loaded.size() == std::size_t{2}) >> fatal) << "expected tool-use history roundtrip";
        const auto *tool = std::get_if<ToolUseBlock>(&loaded[1].content[0]);
        expect((tool != nullptr) >> fatal);
        if (tool != nullptr) {
            expect(tool->id == "tool_123");
            expect(tool->name == "shell");
            expect(tool->input["command"] == "ls -la");
        }
    };

    "save_and_load_tool_result_blocks"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<ContentBlock> result_content;
        result_content.emplace_back(ToolResultBlock{
            .tool_use_id = "tool_123",
            .content = "file1.txt\nfile2.cpp",
            .is_error = false,
        });

        std::vector<Message> messages = {
            {.role = Role::user, .content = std::move(result_content)},
        };

        const auto session_id = store.save(messages, make_session_metadata("test-model"));
        const auto loaded = store.load(session_id);

        expect((loaded.size() == std::size_t{1}) >> fatal) << "expected one persisted tool result";
        const auto *result = std::get_if<ToolResultBlock>(&loaded[0].content[0]);
        expect((result != nullptr) >> fatal);
        if (result != nullptr) {
            expect(result->tool_use_id == "tool_123");
            expect(result->content == "file1.txt\nfile2.cpp");
            expect(not result->is_error);
        }
    };

    "tool_result_preserves_error_flag"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<ContentBlock> content;
        content.emplace_back(ToolResultBlock{
            .tool_use_id = "err_1",
            .content = "command not found",
            .is_error = true,
        });

        std::vector<Message> messages = {{.role = Role::user, .content = std::move(content)}};

        const auto session_id = store.save(messages, make_session_metadata("test-model"));
        const auto loaded = store.load(session_id);

        expect((loaded.size() == std::size_t{1}) >> fatal);
        const auto *result = std::get_if<ToolResultBlock>(&loaded[0].content[0]);
        expect((result != nullptr) >> fatal);
        if (result != nullptr) {
            expect(result->is_error);
        }
    };

    "list_sessions_returns_saved_sessions"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        store.save(messages, make_session_metadata("model-a", "scope:a"));
        store.save(messages, make_session_metadata("model-b", "scope:b"));

        const auto sessions = store.list_sessions();
        expect(sessions.size() == std::size_t{2});

        std::set<std::string> models;
        for (const auto &session : sessions) {
            models.insert(session.model);
            expect(session.message_count == 1);
        }
        expect(models.contains("model-a"));
        expect(models.contains("model-b"));
    };

    "list_sessions_can_filter_by_scope"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto scope_a = store.save(messages, make_session_metadata("model-a", "scope:a"));
        store.save(messages, make_session_metadata("model-b", "scope:b"));

        const auto sessions = store.list_sessions("scope:a");
        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].id == scope_a);
        expect(sessions[0].scope_key == "scope:a");
    };

    "session_belongs_to_scope_checks_ownership"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto session_id = store.save(messages, make_session_metadata("model-a", "scope:a"));

        expect(store.session_belongs_to_scope(session_id, "scope:a"));
        expect(not store.session_belongs_to_scope(session_id, "scope:b"));
    };

    "save_persists_explicit_session_metadata"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

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

        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].id == session_id);
        expect(sessions[0].model == metadata.model);
        expect(sessions[0].scope_key == metadata.scope_key);
        expect(sessions[0].agent_key == metadata.agent_key);
        expect(sessions[0].origin_kind == metadata.origin_kind);
        expect(sessions[0].origin_ref == metadata.origin_ref);
    };

    "create_empty_persists_explicit_session_metadata"_test = [] {
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

        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].id == session_id);
        expect(sessions[0].message_count == 0);
        expect(sessions[0].agent_key == metadata.agent_key);
        expect(sessions[0].origin_kind == metadata.origin_kind);
        expect(sessions[0].origin_ref == metadata.origin_ref);
    };

    "list_sessions_for_agent_returns_only_matching_sessions"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

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
        expect((coder_sessions.size() == std::size_t{1}) >> fatal);
        expect(coder_sessions[0].id == coder_session_id);
        expect(coder_sessions[0].agent_key == "coder");
    };

    "session_belongs_to_agent_checks_ownership"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        const std::vector<Message> messages = {Message::user_text("Hello")};
        const SessionMetadata metadata{
            .model = "test-model",
            .scope_key = "agent:coder",
            .agent_key = "coder",
            .origin_kind = "channel",
            .origin_ref = "qqbot:c2c:alice",
        };
        const auto session_id = store.save(messages, metadata);

        expect(store.session_belongs_to_agent(session_id, "coder"));
        expect(not store.session_belongs_to_agent(session_id, "default"));
    };

    "remove_deletes_session"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("test")};
        const auto id = store.save(messages, make_session_metadata("test-model"));

        expect(store.list_sessions().size() == std::size_t{1});
        store.remove(id);
        expect(store.list_sessions().empty());
        expect(throws<std::runtime_error>([&] {
            static_cast<void>(store.load(id));
        }));
    };

    "load_nonexistent_session_throws"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        expect(throws<std::runtime_error>([&] {
            static_cast<void>(store.load("nonexistent-id"));
        }));
    };

    "create_empty_session_loads_empty_history"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        const auto session_id = store.create_empty(make_session_metadata("claude-sonnet-4-20250514", "scope:a"));
        expect(not session_id.empty());

        const auto loaded = store.load(session_id);
        expect(loaded.empty());

        const auto sessions = store.list_sessions("scope:a");
        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].id == session_id);
        expect(sessions[0].message_count == 0);
    };

    "append_populates_previously_empty_session"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        const auto session_id = store.create_empty(make_session_metadata("claude-sonnet-4-20250514"));
        const std::vector<Message> messages = {
            Message::user_text("Hello"),
            Message::assistant_text("Hi there!"),
        };

        store.append(session_id, messages, 0);

        const auto loaded = store.load(session_id);
        expect((loaded.size() == std::size_t{2}) >> fatal);
        const auto *assistant_text = std::get_if<TextBlock>(&loaded[1].content[0]);
        expect((assistant_text != nullptr) >> fatal);
        if (assistant_text != nullptr) {
            expect(assistant_text->text == "Hi there!");
        }
    };

    "multiple_content_blocks_in_one_message"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<ContentBlock> content;
        content.emplace_back(TextBlock{.text = "I'll run that command"});
        content.emplace_back(ToolUseBlock{
            .id = "call_1",
            .name = "shell",
            .input = json{{"command", "echo hi"}},
        });

        std::vector<Message> messages = {{.role = Role::assistant, .content = std::move(content)}};

        const auto id = store.save(messages, make_session_metadata("test-model"));
        const auto loaded = store.load(id);

        expect((loaded.size() == std::size_t{1}) >> fatal);
        expect((loaded[0].content.size() == std::size_t{2}) >> fatal);
        expect(std::get_if<TextBlock>(&loaded[0].content[0]) != nullptr);
        expect(std::get_if<ToolUseBlock>(&loaded[0].content[1]) != nullptr);
    };

    "latest_session_id_empty_db"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        expect(store.latest_session_id() == std::nullopt);
    };

    "latest_session_id_returns_newest"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        store.save(messages, make_session_metadata("model-a"));
        const auto second_id = store.save(messages, make_session_metadata("model-b"));

        const auto latest = store.latest_session_id();
        expect((latest.has_value()) >> fatal);
        if (latest.has_value()) {
            expect(*latest == second_id);
        }
    };

    "can_bind_and_resolve_session_for_jid"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto session_id = store.save(messages, make_session_metadata("model-a"));

        expect(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
        store.bind_jid("qqbot:c2c:alice", session_id);

        const auto bound = store.bound_session_for_jid("qqbot:c2c:alice");
        expect((bound.has_value()) >> fatal);
        if (bound.has_value()) {
            expect(*bound == session_id);
        }
    };

    "jid_bindings_are_scoped_by_agent_key"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto general_session_id = store.save(messages, make_session_metadata("model-a", "agent:default|jid:qqbot:c2c:alice"));
        const auto coder_session_id = store.save(messages, make_session_metadata("model-b", "agent:coder|jid:qqbot:c2c:alice"));

        store.bind_jid("qqbot:c2c:alice", general_session_id, "default");
        store.bind_jid("qqbot:c2c:alice", coder_session_id, "coder");

        const auto general_bound = store.bound_session_for_jid("qqbot:c2c:alice", "default");
        const auto coder_bound = store.bound_session_for_jid("qqbot:c2c:alice", "coder");

        expect((general_bound.has_value()) >> fatal);
        expect((coder_bound.has_value()) >> fatal);
        if (general_bound.has_value()) {
            expect(*general_bound == general_session_id);
        }
        if (coder_bound.has_value()) {
            expect(*coder_bound == coder_session_id);
        }
    };

    "can_clear_jid_binding"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto session_id = store.save(messages, make_session_metadata("model-a"));
        store.bind_jid("qqbot:c2c:alice", session_id);

        store.clear_jid("qqbot:c2c:alice");
        expect(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
    };

    "removing_session_clears_jid_binding"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        auto messages = std::vector{Message::user_text("Hello")};
        const auto session_id = store.save(messages, make_session_metadata("model-a"));
        store.bind_jid("qqbot:c2c:alice", session_id);

        store.remove(session_id);
        expect(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
    };

    "append_adds_only_new_messages"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<Message> messages = {
            Message::user_text("Hello"),
            Message::assistant_text("Hi there!"),
        };

        const auto session_id = store.save(messages, make_session_metadata("model-a"));
        messages.push_back(Message::user_text("How are you?"));
        messages.push_back(Message::assistant_text("Doing well."));

        store.append(session_id, messages, 2);

        const auto loaded = store.load(session_id);
        expect((loaded.size() == std::size_t{4}) >> fatal);
        const auto *last_text = std::get_if<TextBlock>(&loaded[3].content[0]);
        expect((last_text != nullptr) >> fatal);
        if (last_text != nullptr) {
            expect(last_text->text == "Doing well.");
        }
    };

    "update_and_append_can_refresh_stored_model_metadata"_test = [] {
        SessionStoreHarness harness;
        auto store = harness.store();

        std::vector<Message> messages = {
            Message::user_text("Hello"),
            Message::assistant_text("Hi there!"),
        };

        const auto session_id = store.save(messages, make_session_metadata("model-a", "scope:test"));
        store.update(session_id, messages, "model-b");

        auto sessions = store.list_sessions("scope:test");
        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].model == "model-b");

        messages.push_back(Message::user_text("How are you?"));
        store.append(session_id, messages, 2, "model-c");

        sessions = store.list_sessions("scope:test");
        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].model == "model-c");
    };

    "migrates_legacy_schema_for_scope_and_composite_binding_key"_test = [] {
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
        expect((sessions.size() == std::size_t{1}) >> fatal);
        expect(sessions[0].id == "legacy-session");
        expect(sessions[0].scope_key.empty());

        SqliteDb verify_db(harness.db_path);
        sqlite3_stmt *stmt = nullptr;
        const auto prepare_rc = sqlite3_prepare_v2(verify_db.db, "PRAGMA table_info(channel_session_bindings)", -1, &stmt, nullptr);
        expect((prepare_rc == SQLITE_OK) >> fatal) << "failed to inspect channel_session_bindings schema";

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

        expect(has_agent_key);
        expect(jid_pk_position == 1);
        expect(agent_key_pk_position == 2);

        const auto migrated_binding = store.bound_session_for_jid("qqbot:c2c:alice");
        expect((migrated_binding.has_value()) >> fatal);
        if (migrated_binding.has_value()) {
            expect(*migrated_binding == "legacy-session");
        }
    };
};
