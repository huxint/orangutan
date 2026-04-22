#include "storage/session-store.hpp"
#include "storage/sqlite-throwing.hpp"
#include "types/types.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {

    struct SessionStoreHarness {
        SessionStoreHarness()
        : db_path(orangutan::testing::unique_test_db_path("session-store", "sessions.db")) {}

        ~SessionStoreHarness() {
            std::filesystem::remove_all(db_path.parent_path());
        }
        SessionStoreHarness(const SessionStoreHarness &) = delete;
        SessionStoreHarness &operator=(const SessionStoreHarness &) = delete;
        SessionStoreHarness(SessionStoreHarness &&) = delete;
        SessionStoreHarness &operator=(SessionStoreHarness &&) = delete;

        [[nodiscard]]
        orangutan::SessionStore store() const {
            return orangutan::SessionStore(db_path);
        }

        std::filesystem::path db_path;
    };

    orangutan::SessionMetadata make_session_metadata(std::string model, std::string scope_key = {}) {
        return orangutan::SessionMetadata{
            .model = std::move(model),
            .scope_key = std::move(scope_key),
            .agent_key = "",
            .origin_kind = "cli",
            .origin_ref = "",
        };
    }

    orangutan::PermissionRule make_permission_rule(std::string tool_name, orangutan::permission_behavior behavior, std::optional<orangutan::RuleContent> content = std::nullopt,
                                                   orangutan::permission_rule_source source = orangutan::permission_rule_source::session) {
        return orangutan::PermissionRule{
            .source = source,
            .behavior = behavior,
            .tool_name = std::move(tool_name),
            .content = std::move(content),
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
    REQUIRE(loaded.size() == std::size_t{2});
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
    content.emplace_back(ToolUse("tool_123", "shell", nlohmann::json{{"command", "ls -la"}}));

    std::vector<Message> messages = {
        Message::user().text("list files"),
        Message(base::role::assistant, std::move(content)),
    };

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    INFO("expected tool-use history roundtrip");
    REQUIRE(loaded.size() == std::size_t{2});
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
    result_content.emplace_back(ToolResult{"tool_123", "file1.txt\nfile2.cpp", false});

    std::vector<Message> messages = {
        Message(base::role::user, std::move(result_content)),
    };

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    INFO("expected one persisted tool result");
    REQUIRE(loaded.size() == std::size_t{1});
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
    content.emplace_back(ToolResult{"err_1", "command not found", true});

    std::vector<Message> messages = {Message(base::role::user, std::move(content))};

    const auto session_id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(session_id);

    REQUIRE(loaded.size() == std::size_t{1});
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
    CHECK(sessions.size() == std::size_t{2});

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
    REQUIRE(sessions.size() == std::size_t{1});
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

    REQUIRE(sessions.size() == std::size_t{1});
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

    REQUIRE(sessions.size() == std::size_t{1});
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
    REQUIRE(coder_sessions.size() == std::size_t{1});
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

    CHECK(store.list_sessions().size() == std::size_t{1});
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
    REQUIRE(sessions.size() == std::size_t{1});
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
    REQUIRE(loaded.size() == std::size_t{2});
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
    content.emplace_back(ToolUse("call_1", "shell", nlohmann::json{{"command", "echo hi"}}));

    std::vector<Message> messages = {Message(base::role::assistant, std::move(content))};

    const auto id = store.save(messages, make_session_metadata("test-model"));
    const auto loaded = store.load(id);

    REQUIRE(loaded.size() == std::size_t{1});
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

TEST_CASE("can_save_and_load_session_permission_rules") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));

    store.save_session_permission_rule(session_id, make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));
    store.save_session_permission_rule(
        session_id, make_permission_rule("task", permission_behavior::ask, RuleContent{.match_type = rule_match_type::exact, .pattern = "run"}, permission_rule_source::cli_arg));

    const auto rules = store.load_session_permission_rules(session_id);
    REQUIRE(rules.size() == std::size_t{2});

    CHECK(rules[0].source == permission_rule_source::session);
    CHECK(rules[0].behavior == permission_behavior::allow);
    CHECK(rules[0].tool_name == "shell");
    REQUIRE(rules[0].content.has_value());
    if (rules[0].content.has_value()) {
        CHECK(rules[0].content->match_type == rule_match_type::prefix);
        CHECK(rules[0].content->pattern == "git");
    }

    CHECK(rules[1].source == permission_rule_source::session);
    CHECK(rules[1].behavior == permission_behavior::ask);
    CHECK(rules[1].tool_name == "task");
    REQUIRE(rules[1].content.has_value());
    if (rules[1].content.has_value()) {
        CHECK(rules[1].content->match_type == rule_match_type::exact);
        CHECK(rules[1].content->pattern == "run");
    }
};

TEST_CASE("session_permission_rules_are_scoped_per_session") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto first_session = store.save(messages, make_session_metadata("model-a"));
    const auto second_session = store.save(messages, make_session_metadata("model-b"));

    store.save_session_permission_rule(first_session,
                                       make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));
    store.save_session_permission_rule(second_session,
                                       make_permission_rule("read", permission_behavior::allow, RuleContent{.match_type = rule_match_type::exact, .pattern = "README.md"}));

    const auto first_rules = store.load_session_permission_rules(first_session);
    const auto second_rules = store.load_session_permission_rules(second_session);

    REQUIRE(first_rules.size() == std::size_t{1});
    REQUIRE(second_rules.size() == std::size_t{1});
    CHECK(first_rules[0].tool_name == "shell");
    CHECK(second_rules[0].tool_name == "read");
};

TEST_CASE("load_session_permission_context_replaces_prior_session_rules_only") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));
    store.save_session_permission_rule(session_id, make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));

    ToolPermissionContext context;
    context.allow_rules.push_back(make_permission_rule("read", permission_behavior::allow, std::nullopt, permission_rule_source::user_settings));
    context.allow_rules.push_back(
        make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "stale"}, permission_rule_source::session));
    context.ask_rules.push_back(
        make_permission_rule("task", permission_behavior::ask, RuleContent{.match_type = rule_match_type::exact, .pattern = "run"}, permission_rule_source::session));

    const auto rehydrated = store.load_session_permission_context(session_id, context);
    CHECK(rehydrated.allow_rules.size() == std::size_t{2});
    CHECK(rehydrated.ask_rules.empty());
    CHECK(std::ranges::any_of(rehydrated.allow_rules, [](const PermissionRule &rule) {
        return rule.tool_name == "read" && rule.source == permission_rule_source::user_settings;
    }));
    CHECK(std::ranges::any_of(rehydrated.allow_rules, [](const PermissionRule &rule) {
        return rule.tool_name == "shell" && rule.source == permission_rule_source::session && rule.content.has_value() && rule.content->pattern == "git";
    }));

    const auto cleared = store.load_session_permission_context("", rehydrated);
    CHECK(cleared.ask_rules.empty());
    CHECK(cleared.allow_rules.size() == std::size_t{1});
    CHECK(cleared.allow_rules[0].tool_name == "read");
    CHECK(cleared.allow_rules[0].source == permission_rule_source::user_settings);
};

TEST_CASE("replace_session_permission_rules_persists_only_session_scoped_rules") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));

    ToolPermissionContext context;
    context.allow_rules.push_back(make_permission_rule("read", permission_behavior::allow, std::nullopt, permission_rule_source::user_settings));
    context.allow_rules.push_back(
        make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}, permission_rule_source::session));
    context.ask_rules.push_back(
        make_permission_rule("task", permission_behavior::ask, RuleContent{.match_type = rule_match_type::exact, .pattern = "run"}, permission_rule_source::session));

    store.replace_session_permission_rules(session_id, context);
    auto rules = store.load_session_permission_rules(session_id);
    REQUIRE(rules.size() == std::size_t{2});
    CHECK(std::ranges::none_of(rules, [](const PermissionRule &rule) {
        return rule.tool_name == "read";
    }));
    CHECK(std::ranges::any_of(rules, [](const PermissionRule &rule) {
        return rule.tool_name == "shell" && rule.behavior == permission_behavior::allow;
    }));
    CHECK(std::ranges::any_of(rules, [](const PermissionRule &rule) {
        return rule.tool_name == "task" && rule.behavior == permission_behavior::ask;
    }));

    ToolPermissionContext updated_context;
    updated_context.deny_rules.push_back(
        make_permission_rule("edit", permission_behavior::deny, RuleContent{.match_type = rule_match_type::exact, .pattern = "notes.md"}, permission_rule_source::session));
    store.replace_session_permission_rules(session_id, updated_context);
    rules = store.load_session_permission_rules(session_id);
    REQUIRE(rules.size() == std::size_t{1});
    CHECK(rules[0].tool_name == "edit");
    CHECK(rules[0].behavior == permission_behavior::deny);
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
    store.save_session_permission_rule(session_id, make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));

    store.remove(session_id);
    CHECK(store.bound_session_for_jid("qqbot:c2c:alice") == std::nullopt);
    CHECK(store.load_session_permission_rules(session_id).empty());
};

TEST_CASE("can_clear_session_permission_rules") {
    SessionStoreHarness harness;
    auto store = harness.store();

    auto messages = std::vector{Message::user().text("Hello")};
    const auto session_id = store.save(messages, make_session_metadata("model-a"));
    store.save_session_permission_rule(session_id, make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));

    store.clear_session_permission_rules(session_id);
    CHECK(store.load_session_permission_rules(session_id).empty());
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
    REQUIRE(loaded.size() == std::size_t{4});
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
    REQUIRE(sessions.size() == std::size_t{1});
    CHECK(sessions[0].model == "model-b");

    messages.push_back(Message::user().text("How are you?"));
    store.append(session_id, messages, 2, "model-c");

    sessions = store.list_sessions("scope:test");
    REQUIRE(sessions.size() == std::size_t{1});
    CHECK(sessions[0].model == "model-c");
};

TEST_CASE("migrates_legacy_schema_for_scope_and_composite_binding_key") {
    SessionStoreHarness harness;

    {
        auto db = orangutan::sqlite::open_or_throw(harness.db_path);
        orangutan::sqlite::exec_script(db,
                                       "CREATE TABLE sessions ("
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
        orangutan::sqlite::exec_bind(db, "INSERT INTO sessions (id, model) VALUES (?, ?)", "legacy-session", "legacy-model");
        orangutan::sqlite::exec_bind(db,
                                     "INSERT INTO messages (session_id, seq, role, content_json) VALUES (?, ?, ?, ?)",
                                     "legacy-session",
                                     0,
                                     "user",
                                     R"([{"type":"text","text":"hello"}])");
        orangutan::sqlite::exec_bind(db, "INSERT INTO channel_session_bindings (jid, session_id) VALUES (?, ?)", "qqbot:c2c:alice", "legacy-session");
    }

    auto store = harness.store();

    const auto sessions = store.list_sessions();
    REQUIRE(sessions.size() == std::size_t{1});
    CHECK(sessions[0].id == "legacy-session");
    CHECK(sessions[0].scope_key.empty());

    auto verify_db = orangutan::sqlite::open_or_throw(harness.db_path);
    const auto schema_rows =
        orangutan::sqlite::query_all<std::tuple<std::string, int>>(verify_db, "SELECT name, pk FROM pragma_table_info('channel_session_bindings')");

    bool has_agent_key = false;
    int jid_pk_position = 0;
    int agent_key_pk_position = 0;
    for (const auto &[column_name, pk_position] : schema_rows) {
        if (column_name == "jid") {
            jid_pk_position = pk_position;
        }
        if (column_name == "agent_key") {
            has_agent_key = true;
            agent_key_pk_position = pk_position;
        }
    }

    CHECK(has_agent_key);
    CHECK(jid_pk_position == 1);
    CHECK(agent_key_pk_position == 2);

    const auto migrated_binding = store.bound_session_for_jid("qqbot:c2c:alice");
    REQUIRE(migrated_binding.has_value());
    if (migrated_binding.has_value()) {
        CHECK(*migrated_binding == "legacy-session");
    }

    store.save_session_permission_rule("legacy-session",
                                       make_permission_rule("shell", permission_behavior::allow, RuleContent{.match_type = rule_match_type::prefix, .pattern = "git"}));
    const auto session_rules = store.load_session_permission_rules("legacy-session");
    REQUIRE(session_rules.size() == std::size_t{1});
    CHECK(session_rules[0].tool_name == "shell");
};
