#include "features/memory/memory.hpp"
#include "features/memory/memory-extract.hpp"
#include "features/memory/runtime-memory.hpp"
#include "app/runtime/memory-context.hpp"
#include "core/tools/tool.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using namespace orangutan;

class MemoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_memory_test.db";
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

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

TEST_F(MemoryStoreTest, RememberAndRecallByKeyOrContent) {
    MemoryStore store(db_path().string());
    store.remember("user_name", "Alice", "profile");
    store.remember("project_context", "Working on orangutan", "project");

    const auto by_key = store.recall("user_name");
    EXPECT_NE(by_key.find("Alice"), std::string::npos);

    const auto by_content = store.recall("orangutan");
    EXPECT_NE(by_content.find("project_context"), std::string::npos);
    EXPECT_NE(by_content.find("Working on orangutan"), std::string::npos);
}

TEST_F(MemoryStoreTest, RecallByCategoryReturnsOnlyMatchingEntries) {
    MemoryStore store(db_path().string());
    store.remember("preferred_language", "Python", "preferences");
    store.remember("preferred_editor", "Neovim", "preferences");
    store.remember("project_context", "orangutan", "project");

    const auto entries = store.recall_by_category("preferences");
    ASSERT_EQ(entries.size(), 2);

    std::set<std::pair<std::string, std::string>> actual(entries.begin(), entries.end());
    EXPECT_TRUE(actual.contains({"preferred_language", "Python"}));
    EXPECT_TRUE(actual.contains({"preferred_editor", "Neovim"}));
    EXPECT_FALSE(actual.contains({"project_context", "orangutan"}));
}

TEST_F(MemoryStoreTest, SearchReturnsMostRelevantEntriesFirst) {
    MemoryStore store(db_path().string());
    store.remember("project.current", "orangutan memory refactor", "project");
    store.remember("misc.note", "buy groceries later", "general");

    const auto results = store.search("project.current");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.front().key, "project.current");
}

TEST_F(MemoryStoreTest, SearchBackfillsPastCategoryCapWhenOnlyOneCategoryMatches) {
    MemoryStore store(db_path().string());
    store.remember("project.alpha", "shared project memory alpha", "project");
    store.remember("project.beta", "shared project memory beta", "project");
    store.remember("project.gamma", "shared project memory gamma", "project");
    store.remember("project.delta", "shared project memory delta", "project");

    const auto results = store.search("shared project memory", {}, 4);
    ASSERT_EQ(results.size(), 4U);

    std::set<std::string> keys;
    for (const auto &record : results) {
        EXPECT_EQ(record.category, "project");
        keys.insert(record.key);
    }

    EXPECT_TRUE(keys.contains("project.alpha"));
    EXPECT_TRUE(keys.contains("project.beta"));
    EXPECT_TRUE(keys.contains("project.gamma"));
    EXPECT_TRUE(keys.contains("project.delta"));
}

TEST_F(MemoryStoreTest, ForgetRemovesExistingEntry) {
    MemoryStore store(db_path().string());
    store.remember("user_name", "Alice", "profile");

    EXPECT_TRUE(store.forget("user_name"));
    EXPECT_FALSE(store.forget("user_name"));
    EXPECT_TRUE(store.recall_by_category("profile").empty());
}

TEST_F(MemoryStoreTest, RememberUpdatesExistingKey) {
    MemoryStore store(db_path().string());
    store.remember("user_name", "Alice", "profile");
    store.remember("user_name", "Bob", "profile");

    const auto recall = store.recall("user_name");
    EXPECT_NE(recall.find("Bob"), std::string::npos);
    EXPECT_EQ(recall.find("Alice"), std::string::npos);
}

TEST_F(MemoryStoreTest, UpdateCanMergeDistinctContentFragments) {
    MemoryStore store(db_path().string());
    store.remember("preference.general", "concise answers", "preference");
    store.update("preference.general", "short commit messages", "preference");

    const auto recall = store.recall("preference.general");
    EXPECT_NE(recall.find("concise answers"), std::string::npos);
    EXPECT_NE(recall.find("short commit messages"), std::string::npos);
}

TEST_F(MemoryStoreTest, UpdatePreservesExistingCategoryAndSourceWhenOmitted) {
    MemoryStore store(db_path().string());
    store.remember("profile.name", "Alice", "profile", {}, "auto:session", 0.9);

    store.update("profile.name", "Alice Example");

    const auto entries = store.list({}, "profile", 10);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].content, "Alice Example");
    EXPECT_EQ(entries[0].category, "profile");
    EXPECT_EQ(entries[0].source, "auto:session");

    const auto stats = store.stats();
    EXPECT_EQ(stats.auto_entries, 1);
    EXPECT_EQ(stats.manual_entries, 0);
}

TEST_F(MemoryStoreTest, DumpAllIncludesStoredMemories) {
    MemoryStore store(db_path().string());
    store.remember("user_name", "Alice", "profile");
    store.remember("project_context", "Working on orangutan", "project");

    const auto dump = store.dump_all();
    EXPECT_NE(dump.find("user_name"), std::string::npos);
    EXPECT_NE(dump.find("Alice"), std::string::npos);
    EXPECT_NE(dump.find("project_context"), std::string::npos);
    EXPECT_NE(dump.find("Working on orangutan"), std::string::npos);
}

TEST_F(MemoryStoreTest, StatsCountManualAndAutoEntries) {
    MemoryStore store(db_path().string());
    store.remember("user_name", "Alice", "profile", {}, "manual", 0.9);
    (void)store.auto_capture("my name is Bob and we are working on orangutan");

    const auto stats = store.stats();
    EXPECT_EQ(stats.total, 3);
    EXPECT_GE(stats.categories, 2);
    EXPECT_EQ(stats.manual_entries, 1);
    EXPECT_EQ(stats.auto_entries, 2);
}

TEST_F(MemoryStoreTest, AutoCaptureExtractsStructuredMemories) {
    MemoryStore store(db_path().string());
    (void)store.auto_capture("my name is Alice. We are working on orangutan. I prefer concise answers.");

    const auto name = store.recall("profile.name");
    const auto project = store.recall("project.current");
    const auto preference = store.recall("preference.general");

    EXPECT_NE(name.find("Alice"), std::string::npos);
    EXPECT_NE(project.find("orangutan"), std::string::npos);
    EXPECT_NE(preference.find("concise answers"), std::string::npos);
}

TEST_F(MemoryStoreTest, AutoCaptureMergesRepeatedGeneralPreferences) {
    MemoryStore store(db_path().string());
    (void)store.auto_capture("I prefer concise answers.");
    (void)store.auto_capture("I prefer short commit messages.");

    const auto preference = store.recall("preference.general");
    EXPECT_NE(preference.find("concise answers"), std::string::npos);
    EXPECT_NE(preference.find("short commit messages"), std::string::npos);
}

TEST_F(MemoryStoreTest, ScopedMemoriesDoNotLeakAcrossIdentities) {
    MemoryStore store(db_path().string());
    store.remember("preferred_name", "Alice", "profile", "jid:alice");
    store.remember("preferred_name", "Bob", "profile", "jid:bob");

    const auto alice = store.recall("preferred_name", "jid:alice");
    const auto bob = store.recall("preferred_name", "jid:bob");
    const auto global = store.dump_all();

    EXPECT_NE(alice.find("Alice"), std::string::npos);
    EXPECT_EQ(alice.find("Bob"), std::string::npos);
    EXPECT_NE(bob.find("Bob"), std::string::npos);
    EXPECT_EQ(bob.find("Alice"), std::string::npos);
    EXPECT_TRUE(global.empty());
}

TEST_F(MemoryStoreTest, BuiltinMemoryToolsRegisterAndExecute) {
    MemoryStore store(db_path().string());
    ToolRegistry registry;
    RuntimeMemory runtime_memory(store);
    register_builtin_tools(registry, &runtime_memory);

    const auto definitions = registry.definitions();
    std::set<std::string> tool_names;
    for (const auto &definition : definitions) {
        tool_names.insert(definition.name);
    }

    EXPECT_TRUE(tool_names.contains("remember"));
    EXPECT_TRUE(tool_names.contains("recall"));
    EXPECT_TRUE(tool_names.contains("forget"));
    EXPECT_TRUE(tool_names.contains("memory_store"));
    EXPECT_TRUE(tool_names.contains("memory_recall"));
    EXPECT_TRUE(tool_names.contains("memory_forget"));
    EXPECT_TRUE(tool_names.contains("memory_update"));
    EXPECT_TRUE(tool_names.contains("memory_list"));
    EXPECT_TRUE(tool_names.contains("memory_stats"));

    auto remember_result = registry.execute(ToolUseBlock{
        .id = "remember-1",
        .name = "remember",
        .input = {{"key", "preferred_language"}, {"content", "C++"}, {"category", "preferences"}},
    });
    EXPECT_FALSE(remember_result.is_error);

    auto recall_result = registry.execute(ToolUseBlock{
        .id = "recall-1",
        .name = "recall",
        .input = {{"query", "preferred_language"}},
    });
    EXPECT_FALSE(recall_result.is_error);
    EXPECT_NE(recall_result.content.find("C++"), std::string::npos);

    auto forget_result = registry.execute(ToolUseBlock{
        .id = "forget-1",
        .name = "forget",
        .input = {{"key", "preferred_language"}},
    });
    EXPECT_FALSE(forget_result.is_error);
    EXPECT_NE(forget_result.content.find("Forgot"), std::string::npos);
}

TEST_F(MemoryStoreTest, PluginStyleMemoryAliasesWork) {
    MemoryStore store(db_path().string());
    ToolRegistry registry;
    RuntimeMemory runtime_memory(store);
    register_builtin_tools(registry, &runtime_memory);

    auto store_result = registry.execute(ToolUseBlock{
        .id = "memory-store-1",
        .name = "memory_store",
        .input = {{"key", "profile.name"}, {"content", "Alice"}, {"category", "profile"}},
    });
    auto recall_result = registry.execute(ToolUseBlock{
        .id = "memory-recall-1",
        .name = "memory_recall",
        .input = {{"query", "profile.name"}},
    });
    auto update_result = registry.execute(ToolUseBlock{
        .id = "memory-update-1",
        .name = "memory_update",
        .input = {{"key", "profile.name"}, {"content", "Alice Example"}, {"category", "profile"}, {"merge", false}},
    });
    auto stats_result = registry.execute(ToolUseBlock{
        .id = "memory-stats-1",
        .name = "memory_stats",
        .input = json::object(),
    });
    auto list_result = registry.execute(ToolUseBlock{
        .id = "memory-list-1",
        .name = "memory_list",
        .input = {{"category", "profile"}},
    });

    EXPECT_FALSE(store_result.is_error);
    EXPECT_FALSE(recall_result.is_error);
    EXPECT_FALSE(update_result.is_error);
    EXPECT_FALSE(stats_result.is_error);
    EXPECT_FALSE(list_result.is_error);
    EXPECT_NE(recall_result.content.find("Alice"), std::string::npos);
    EXPECT_NE(stats_result.content.find("total=1"), std::string::npos);
    EXPECT_NE(list_result.content.find("Alice Example"), std::string::npos);
}

TEST_F(MemoryStoreTest, BuiltinMemoryToolsRespectMemoryScope) {
    MemoryStore store(db_path().string());
    ToolRegistry alice_registry;
    ToolRegistry bob_registry;
    RuntimeMemory alice_memory(store, RuntimeMemoryContext{.scope = "jid:alice"});
    RuntimeMemory bob_memory(store, RuntimeMemoryContext{.scope = "jid:bob"});
    register_builtin_tools(alice_registry, &alice_memory);
    register_builtin_tools(bob_registry, &bob_memory);

    auto alice_remember = alice_registry.execute(ToolUseBlock{
        .id = "remember-alice",
        .name = "remember",
        .input = {{"key", "favorite_color"}, {"content", "green"}},
    });
    auto bob_recall = bob_registry.execute(ToolUseBlock{
        .id = "recall-bob",
        .name = "recall",
        .input = {{"query", "favorite_color"}},
    });
    auto alice_recall = alice_registry.execute(ToolUseBlock{
        .id = "recall-alice",
        .name = "recall",
        .input = {{"query", "favorite_color"}},
    });

    EXPECT_FALSE(alice_remember.is_error);
    EXPECT_FALSE(bob_recall.is_error);
    EXPECT_FALSE(alice_recall.is_error);
    EXPECT_EQ(bob_recall.content, "(no memories found)");
    EXPECT_NE(alice_recall.content.find("green"), std::string::npos);
}

TEST_F(MemoryStoreTest, MigratesLegacyMemoryTableToStructuredSchema) {
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &db), SQLITE_OK);

    char *err_msg = nullptr;
    ASSERT_EQ(sqlite3_exec(db,
                           R"(
                               CREATE TABLE memories (
                                   id INTEGER PRIMARY KEY AUTOINCREMENT,
                                   key TEXT NOT NULL UNIQUE,
                                   content TEXT NOT NULL,
                                   category TEXT NOT NULL DEFAULT 'general',
                                   created_at TEXT NOT NULL DEFAULT (datetime('now')),
                                   updated_at TEXT NOT NULL DEFAULT (datetime('now'))
                               );
                           )",
                           nullptr, nullptr, &err_msg),
              SQLITE_OK)
        << (err_msg != nullptr ? err_msg : "sqlite error");
    sqlite3_free(err_msg);

    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO memories (key, content, category) VALUES ('s:jid:alice\x1fpreferred_name', 'Alice', 'profile');", nullptr, nullptr, &err_msg),
              SQLITE_OK)
        << (err_msg != nullptr ? err_msg : "sqlite error");
    sqlite3_free(err_msg);
    sqlite3_close(db);

    MemoryStore store(db_path().string());
    const auto recall = store.recall("preferred_name", "jid:alice");
    EXPECT_NE(recall.find("Alice"), std::string::npos);
}

// --- UTF-8 sanitization unit tests ---

using orangutan::memory_detail::sanitize_utf8;

TEST(SanitizeUtf8Test, AsciiPassesThrough) {
    EXPECT_EQ(sanitize_utf8("hello world"), "hello world");
    EXPECT_EQ(sanitize_utf8(""), "");
}

TEST(SanitizeUtf8Test, ValidChinesePassesThrough) {
    // "算法高手" — four 3-byte CJK characters (12 bytes total)
    const std::string input = "\xe7\xae\x97\xe6\xb3\x95\xe9\xab\x98\xe6\x89\x8b";
    EXPECT_EQ(sanitize_utf8(input), input);
}

TEST(SanitizeUtf8Test, TruncatedChineseCharacterIsStripped) {
    // "算" is e7 ae 97. Slice off the last byte → e7 ae (incomplete).
    const std::string truncated = "\xe7\xae";
    EXPECT_EQ(sanitize_utf8(truncated), "");
}

TEST(SanitizeUtf8Test, ValidTextFollowedByTruncatedCharacter) {
    // "hi" + first 2 bytes of a 3-byte char
    const std::string input = std::string("hi") + "\xe4\xb8";
    EXPECT_EQ(sanitize_utf8(input), "hi");
}

TEST(SanitizeUtf8Test, ValidChineseFollowedByTruncatedCharacter) {
    // "算" (e7 ae 97) + first byte of next char (e6)
    const std::string input = "\xe7\xae\x97\xe6";
    EXPECT_EQ(sanitize_utf8(input), "\xe7\xae\x97");
}

TEST(SanitizeUtf8Test, AccentedLatinPassesThrough) {
    // "José" — the é is c3 a9 (2-byte UTF-8)
    const std::string input = "Jos\xc3\xa9";
    EXPECT_EQ(sanitize_utf8(input), input);
}

TEST(SanitizeUtf8Test, TruncatedAccentedLatinIsStripped) {
    // "Jos" + first byte of é (c3, missing a9)
    const std::string input = "Jos\xc3";
    EXPECT_EQ(sanitize_utf8(input), "Jos");
}

TEST(SanitizeUtf8Test, EmojiPassesThrough) {
    // 🚀 = f0 9f 9a 80 (4-byte UTF-8)
    const std::string input = "\xf0\x9f\x9a\x80";
    EXPECT_EQ(sanitize_utf8(input), input);
}

TEST(SanitizeUtf8Test, TruncatedEmojiIsStripped) {
    // 🚀 with last byte missing: f0 9f 9a
    const std::string input = "go\xf0\x9f\x9a";
    EXPECT_EQ(sanitize_utf8(input), "go");
}

TEST(SanitizeUtf8Test, StrayContinuationBytesAreSkipped) {
    // Stray continuation bytes (0x80-0xBF) without a lead byte
    const std::string input = std::string("ok") + "\x80\xbf" + "fine";
    EXPECT_EQ(sanitize_utf8(input), "okfine");
}

// --- Auto-capture UTF-8 integration tests ---

TEST_F(MemoryStoreTest, AutoCaptureChineseNameProducesValidUtf8) {
    MemoryStore store(db_path().string());
    (void)store.auto_capture("我是一个算法高手");

    const auto name = store.recall("profile.name");
    // Must not throw when serialized to JSON.
    EXPECT_NO_THROW({
        nlohmann::json j = name;
        (void)j.dump();
    });
    // The captured content should contain valid Chinese text.
    if (name.find("(no memories found)") == std::string::npos) {
        EXPECT_NE(name.find("\xe4\xb8\x80"), std::string::npos); // "一" present
    }
}

TEST_F(MemoryStoreTest, AutoCaptureChineseRememberProducesValidUtf8) {
    MemoryStore store(db_path().string());
    (void)store.auto_capture("记住我是一个算法高手");

    const auto stats = store.stats();
    EXPECT_GE(stats.auto_entries, 1);

    // Verify all stored content is JSON-serializable (valid UTF-8).
    const auto dump = store.dump_all();
    EXPECT_NO_THROW({
        nlohmann::json j = dump;
        (void)j.dump();
    });
}

TEST_F(MemoryStoreTest, AutoCaptureAccentedLatinProducesValidUtf8) {
    MemoryStore store(db_path().string());
    (void)store.auto_capture("my name is Jos\xc3\xa9");

    const auto name = store.recall("profile.name");
    EXPECT_NO_THROW({
        nlohmann::json j = name;
        (void)j.dump();
    });
    EXPECT_NE(name.find("Jos\xc3\xa9"), std::string::npos);
}

TEST_F(MemoryStoreTest, AutoCaptureEmojiProducesValidUtf8) {
    MemoryStore store(db_path().string());
    // "remember that the deploy emoji is 🚀"
    (void)store.auto_capture("remember that the deploy emoji is \xf0\x9f\x9a\x80");

    const auto stats = store.stats();
    EXPECT_GE(stats.auto_entries, 1);

    const auto dump = store.dump_all();
    EXPECT_NO_THROW({
        nlohmann::json j = dump;
        (void)j.dump();
    });
}


TEST_F(MemoryStoreTest, StatsExposeJournalEntriesSeparately) {
    MemoryStore store(db_path().string());
    store.remember("project.current", "orangutan memory refactor", "project", {}, "session:distilled", 0.9);
    store.remember("journal.1", "Reviewed recall ranking and mirror design", "journal", {}, "session:journal", 0.4);

    const auto stats = store.stats();
    EXPECT_EQ(stats.total, 2);
    EXPECT_EQ(stats.journal_entries, 1);
}

TEST_F(MemoryStoreTest, RuntimeMemoryRefreshesManagedSnapshotForDurableMemories) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_snapshot_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    MemoryStore store(db_path().string());
    RuntimeMemory runtime(store, RuntimeMemoryContext{
                                   .scope = "scope:test",
                                   .workspace = workspace.string(),
                                   .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                               });

    runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

    const auto snapshot = workspace / "MEMORY.md";
    ASSERT_TRUE(std::filesystem::exists(snapshot));
    const auto text = read_text_file(snapshot);
    EXPECT_NE(text.find("<!-- ORANGUTAN:MEMORY:BEGIN -->"), std::string::npos);
    EXPECT_NE(text.find("orangutan memory enhancements"), std::string::npos);
    EXPECT_EQ(text.find("session:journal"), std::string::npos);

    std::filesystem::remove_all(workspace);
}

TEST_F(MemoryStoreTest, RuntimeMemoryDisabledMirrorDoesNotCreateSnapshot) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_disabled_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    MemoryStore store(db_path().string());
    RuntimeMemory runtime(store, RuntimeMemoryContext{
                                   .scope = "scope:test",
                                   .workspace = workspace.string(),
                                   .mirror = {.enabled = false, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                               });

    runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);
    EXPECT_FALSE(std::filesystem::exists(workspace / "MEMORY.md"));

    std::filesystem::remove_all(workspace);
}

TEST_F(MemoryStoreTest, RuntimeMemorySkipsMalformedManagedSection) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_invalid_layout_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    const auto snapshot = workspace / "MEMORY.md";

    {
        std::ofstream out(snapshot);
        out << "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n";
    }

    MemoryStore store(db_path().string());
    RuntimeMemory runtime(store, RuntimeMemoryContext{
                                   .scope = "scope:test",
                                   .workspace = workspace.string(),
                                   .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                               });
    runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

    const auto refresh = runtime.refresh_mirror();
    EXPECT_TRUE(refresh.skipped);
    EXPECT_EQ(read_text_file(snapshot), "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n");

    std::filesystem::remove_all(workspace);
}

TEST_F(MemoryStoreTest, RuntimeMemorySkipsRefreshWhenManagedMarkersAreDuplicated) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_duplicate_marker_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    const auto snapshot = workspace / "MEMORY.md";

    const std::string original =
        "User notes\n"
        "<!-- ORANGUTAN:MEMORY:BEGIN -->\n"
        "old generated block\n"
        "<!-- ORANGUTAN:MEMORY:END -->\n"
        "stray marker <!-- ORANGUTAN:MEMORY:BEGIN -->\n";
    {
        std::ofstream out(snapshot);
        out << original;
    }

    MemoryStore store(db_path().string());
    RuntimeMemory runtime(store, RuntimeMemoryContext{
                                   .scope = "scope:test",
                                   .workspace = workspace.string(),
                                   .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                               });
    runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

    const auto refresh = runtime.refresh_mirror();
    EXPECT_TRUE(refresh.skipped);
    EXPECT_EQ(read_text_file(snapshot), original);

    std::filesystem::remove_all(workspace);
}

TEST_F(MemoryStoreTest, RuntimeMemoryStoresJournalSummaryAndAppendsDailyJournalFile) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_journal_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    MemoryStore store(db_path().string());
    RuntimeMemory runtime(store, RuntimeMemoryContext{
                                   .scope = "scope:test",
                                   .workspace = workspace.string(),
                                   .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                               });

    const auto result = runtime.store_journal_summary("Reviewed ranking and mirror behavior.");
    EXPECT_TRUE(result.stored);
    EXPECT_TRUE(result.mirrored);

    const auto journal_entries = runtime.list("journal", 10);
    ASSERT_EQ(journal_entries.size(), 1U);
    EXPECT_EQ(journal_entries[0].source, "session:journal");
    EXPECT_NE(journal_entries[0].content.find("Reviewed ranking and mirror behavior."), std::string::npos);

    const auto journal_root = workspace / "memory";
    ASSERT_TRUE(std::filesystem::exists(journal_root));
    auto it = std::filesystem::directory_iterator(journal_root);
    ASSERT_NE(it, std::filesystem::directory_iterator{});
    const auto journal_text = read_text_file(it->path());
    EXPECT_NE(journal_text.find("Reviewed ranking and mirror behavior."), std::string::npos);

    std::filesystem::remove_all(workspace);
}

TEST_F(MemoryStoreTest, SearchFallsBackForNonAsciiQueries) {
    MemoryStore store(db_path().string());
    store.remember("fact.algorithms", "算法高手", "profile");

    const auto results = store.search("算法", {}, 4);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.front().key, "fact.algorithms");
}

TEST_F(MemoryStoreTest, RuntimeMemoryMirrorRefreshIsScopeAware) {
    const auto workspace = std::filesystem::temp_directory_path() / "orangutan_runtime_memory_scope_test";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);

    MemoryStore store(db_path().string());
    RuntimeMemory alpha(store, RuntimeMemoryContext{
                              .scope = "scope:alpha",
                              .workspace = workspace.string(),
                              .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                          });
    RuntimeMemory beta(store, RuntimeMemoryContext{
                             .scope = "scope:beta",
                             .workspace = workspace.string(),
                             .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                         });

    alpha.remember("project.current", "alpha memory", "project", "session:distilled", 0.9);
    beta.remember("project.current", "beta memory", "project", "session:distilled", 0.9);

    const auto alpha_refresh = alpha.refresh_mirror();
    EXPECT_TRUE(alpha_refresh.refreshed);
    auto snapshot = read_text_file(workspace / "MEMORY.md");
    EXPECT_NE(snapshot.find("alpha memory"), std::string::npos);
    EXPECT_EQ(snapshot.find("beta memory"), std::string::npos);

    const auto beta_refresh = beta.refresh_mirror();
    EXPECT_TRUE(beta_refresh.refreshed);
    snapshot = read_text_file(workspace / "MEMORY.md");
    EXPECT_NE(snapshot.find("beta memory"), std::string::npos);
    EXPECT_EQ(snapshot.find("alpha memory"), std::string::npos);

    std::filesystem::remove_all(workspace);
}