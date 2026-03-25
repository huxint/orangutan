#include "features/memory/memory.hpp"
#include "features/memory/memory-extract.hpp"
#include "features/memory/runtime-memory.hpp"
#include "app/runtime/memory-context.hpp"
#include "core/tools/tool.hpp"
#include "infra/utf8.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include "support/ut.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using namespace orangutan;

namespace {

class MemoryStoreHarness {
public:
    MemoryStoreHarness()
    : db_path_(orangutan::testing::unique_test_db_path("memory-store", "memory.db")) {}

    ~MemoryStoreHarness() {
        std::filesystem::remove_all(db_path_.parent_path());
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

template <typename Fn>
bool completes_without_throw(Fn &&fn) {
    try {
        std::forward<Fn>(fn)();
        return true;
    } catch (...) {
        return false;
    }
}

boost::ut::suite memory_store_suite = [] {
    using namespace boost::ut;

    "remember_and_recall_by_key_or_content"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("project_context", "Working on orangutan", "project");

        const auto by_key = store.recall("user_name");
        expect(by_key.find("Alice") != std::string::npos);

        const auto by_content = store.recall("orangutan");
        expect(by_content.find("project_context") != std::string::npos);
        expect(by_content.find("Working on orangutan") != std::string::npos);
    };

    "recall_by_category_returns_only_matching_entries"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preferred_language", "Python", "preferences");
        store.remember("preferred_editor", "Neovim", "preferences");
        store.remember("project_context", "orangutan", "project");

        const auto entries = store.recall_by_category("preferences");
        expect((entries.size() == 2_ul) >> fatal);

        std::set<std::pair<std::string, std::string>> actual(entries.begin(), entries.end());
        expect(actual.contains({"preferred_language", "Python"}));
        expect(actual.contains({"preferred_editor", "Neovim"}));
        expect(not actual.contains({"project_context", "orangutan"}));
    };

    "search_returns_most_relevant_entries_first"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project");
        store.remember("misc.note", "buy groceries later", "general");

        const auto results = store.search("project.current");
        expect((not results.empty()) >> fatal);
        expect(results.front().key == "project.current");
    };

    "search_backfills_past_category_cap_when_only_one_category_matches"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.alpha", "shared project memory alpha", "project");
        store.remember("project.beta", "shared project memory beta", "project");
        store.remember("project.gamma", "shared project memory gamma", "project");
        store.remember("project.delta", "shared project memory delta", "project");

        const auto results = store.search("shared project memory", {}, 4);
        expect((results.size() == 4_ul) >> fatal);

        std::set<std::string> keys;
        for (const auto &record : results) {
            expect(record.category == "project");
            keys.insert(record.key);
        }

        expect(keys.contains("project.alpha"));
        expect(keys.contains("project.beta"));
        expect(keys.contains("project.gamma"));
        expect(keys.contains("project.delta"));
    };

    "forget_removes_existing_entry"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");

        expect(store.forget("user_name"));
        expect(not store.forget("user_name"));
        expect(store.recall_by_category("profile").empty());
    };

    "remember_updates_existing_key"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("user_name", "Bob", "profile");

        const auto recall = store.recall("user_name");
        expect(recall.find("Bob") != std::string::npos);
        expect(recall.find("Alice") == std::string::npos);
    };

    "update_can_merge_distinct_content_fragments"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preference.general", "concise answers", "preference");
        store.update("preference.general", "short commit messages", "preference");

        const auto recall = store.recall("preference.general");
        expect(recall.find("concise answers") != std::string::npos);
        expect(recall.find("short commit messages") != std::string::npos);
    };

    "update_preserves_existing_category_and_source_when_omitted"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("profile.name", "Alice", "profile", {}, "auto:session", 0.9);

        store.update("profile.name", "Alice Example");

        const auto entries = store.list({}, "profile", 10);
        expect((entries.size() == 1_ul) >> fatal);
        expect(entries[0].content == "Alice Example");
        expect(entries[0].category == "profile");
        expect(entries[0].source == "auto:session");

        const auto stats = store.stats();
        expect(stats.auto_entries == 1_i);
        expect(stats.manual_entries == 0_i);
    };

    "dump_all_includes_stored_memories"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile");
        store.remember("project_context", "Working on orangutan", "project");

        const auto dump = store.dump_all();
        expect(dump.find("user_name") != std::string::npos);
        expect(dump.find("Alice") != std::string::npos);
        expect(dump.find("project_context") != std::string::npos);
        expect(dump.find("Working on orangutan") != std::string::npos);
    };

    "stats_count_manual_and_auto_entries"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("user_name", "Alice", "profile", {}, "manual", 0.9);
        static_cast<void>(store.auto_capture("my name is Bob and we are working on orangutan"));

        const auto stats = store.stats();
        expect(stats.total == 3_i);
        expect(stats.categories >= 2_i);
        expect(stats.manual_entries == 1_i);
        expect(stats.auto_entries == 2_i);
    };

    "auto_capture_extracts_structured_memories"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("my name is Alice. We are working on orangutan. I prefer concise answers."));

        const auto name = store.recall("profile.name");
        const auto project = store.recall("project.current");
        const auto preference = store.recall("preference.general");

        expect(name.find("Alice") != std::string::npos);
        expect(project.find("orangutan") != std::string::npos);
        expect(preference.find("concise answers") != std::string::npos);
    };

    "auto_capture_merges_repeated_general_preferences"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("I prefer concise answers."));
        static_cast<void>(store.auto_capture("I prefer short commit messages."));

        const auto preference = store.recall("preference.general");
        expect(preference.find("concise answers") != std::string::npos);
        expect(preference.find("short commit messages") != std::string::npos);
    };

    "scoped_memories_do_not_leak_across_identities"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("preferred_name", "Alice", "profile", "jid:alice");
        store.remember("preferred_name", "Bob", "profile", "jid:bob");

        const auto alice = store.recall("preferred_name", "jid:alice");
        const auto bob = store.recall("preferred_name", "jid:bob");
        const auto global = store.dump_all();

        expect(alice.find("Alice") != std::string::npos);
        expect(alice.find("Bob") == std::string::npos);
        expect(bob.find("Bob") != std::string::npos);
        expect(bob.find("Alice") == std::string::npos);
        expect(global.empty());
    };

    "builtin_memory_tools_register_and_execute"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        ToolRegistry registry;
        RuntimeMemory runtime_memory(store);
        register_builtin_tools(registry, &runtime_memory);

        const auto definitions = registry.definitions();
        std::set<std::string> tool_names;
        for (const auto &definition : definitions) {
            tool_names.insert(definition.name);
        }

        expect(tool_names.contains("remember"));
        expect(tool_names.contains("recall"));
        expect(tool_names.contains("forget"));
        expect(tool_names.contains("memory_store"));
        expect(tool_names.contains("memory_recall"));
        expect(tool_names.contains("memory_forget"));
        expect(tool_names.contains("memory_update"));
        expect(tool_names.contains("memory_list"));
        expect(tool_names.contains("memory_stats"));

        auto remember_result = registry.execute(ToolUseBlock{
            .id = "remember-1",
            .name = "remember",
            .input = {{"key", "preferred_language"}, {"content", "C++"}, {"category", "preferences"}},
        });
        expect(not remember_result.is_error);

        auto recall_result = registry.execute(ToolUseBlock{
            .id = "recall-1",
            .name = "recall",
            .input = {{"query", "preferred_language"}},
        });
        expect(not recall_result.is_error);
        expect(recall_result.content.find("C++") != std::string::npos);

        auto forget_result = registry.execute(ToolUseBlock{
            .id = "forget-1",
            .name = "forget",
            .input = {{"key", "preferred_language"}},
        });
        expect(not forget_result.is_error);
        expect(forget_result.content.find("Forgot") != std::string::npos);
    };

    "plugin_style_memory_aliases_work"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
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

        expect(not store_result.is_error);
        expect(not recall_result.is_error);
        expect(not update_result.is_error);
        expect(not stats_result.is_error);
        expect(not list_result.is_error);
        expect(recall_result.content.find("Alice") != std::string::npos);
        expect(stats_result.content.find("total=1") != std::string::npos);
        expect(list_result.content.find("Alice Example") != std::string::npos);
    };

    "builtin_memory_tools_respect_memory_scope"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
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

        expect(not alice_remember.is_error);
        expect(not bob_recall.is_error);
        expect(not alice_recall.is_error);
        expect(bob_recall.content == "(no memories found)");
        expect(alice_recall.content.find("green") != std::string::npos);
    };

    "migrates_legacy_memory_table_to_structured_schema"_test = [] {
        MemoryStoreHarness harness;
        sqlite3 *db = nullptr;
        const auto open_status = sqlite3_open(harness.db_path().string().c_str(), &db);
        expect((open_status == SQLITE_OK) >> fatal);

        char *err_msg = nullptr;
        const auto create_status = sqlite3_exec(db,
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
                                                nullptr, nullptr, &err_msg);
        expect((create_status == SQLITE_OK) >> fatal) << (err_msg != nullptr ? err_msg : "sqlite error");
        sqlite3_free(err_msg);
        err_msg = nullptr;

        const auto insert_status =
            sqlite3_exec(db, "INSERT INTO memories (key, content, category) VALUES ('s:jid:alice\x1fpreferred_name', 'Alice', 'profile');", nullptr, nullptr, &err_msg);
        expect((insert_status == SQLITE_OK) >> fatal) << (err_msg != nullptr ? err_msg : "sqlite error");
        sqlite3_free(err_msg);
        sqlite3_close(db);

        MemoryStore store(harness.db_path());
        const auto recall = store.recall("preferred_name", "jid:alice");
        expect(recall.find("Alice") != std::string::npos);
    };

    "auto_capture_chinese_name_produces_valid_utf8"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("我是一个算法高手"));

        const auto name = store.recall("profile.name");
        expect(completes_without_throw([&] {
            nlohmann::json j = name;
            static_cast<void>(j.dump());
        }));
        if (!name.contains("(no memories found)")) {
            expect(name.contains("\xe4\xb8\x80"));
        }
    };

    "auto_capture_chinese_remember_produces_valid_utf8"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("记住我是一个算法高手"));

        const auto stats = store.stats();
        expect(stats.auto_entries >= 1_i);

        const auto dump = store.dump_all();
        expect(completes_without_throw([&] {
            nlohmann::json j = dump;
            static_cast<void>(j.dump());
        }));
    };

    "auto_capture_accented_latin_produces_valid_utf8"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("my name is Jos\xc3\xa9"));

        const auto name = store.recall("profile.name");
        expect(completes_without_throw([&] {
            nlohmann::json j = name;
            static_cast<void>(j.dump());
        }));
        expect(name.find("Jos\xc3\xa9") != std::string::npos);
    };

    "auto_capture_emoji_produces_valid_utf8"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        static_cast<void>(store.auto_capture("remember that the deploy emoji is \xf0\x9f\x9a\x80"));

        const auto stats = store.stats();
        expect(stats.auto_entries >= 1_i);

        const auto dump = store.dump_all();
        expect(completes_without_throw([&] {
            nlohmann::json j = dump;
            static_cast<void>(j.dump());
        }));
    };

    "stats_expose_journal_entries_separately"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("project.current", "orangutan memory refactor", "project", {}, "session:distilled", 0.9);
        store.remember("journal.1", "Reviewed recall ranking and mirror design", "journal", {}, "session:journal", 0.4);

        const auto stats = store.stats();
        expect(stats.total == 2_i);
        expect(stats.journal_entries == 1_i);
    };

    "runtime_memory_refreshes_managed_snapshot_for_durable_memories"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-snapshot");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto snapshot = workspace / "MEMORY.md";
        expect((std::filesystem::exists(snapshot)) >> fatal);
        const auto text = read_text_file(snapshot);
        expect(text.find("<!-- ORANGUTAN:MEMORY:BEGIN -->") != std::string::npos);
        expect(text.find("orangutan memory enhancements") != std::string::npos);
        expect(text.find("session:journal") == std::string::npos);

        std::filesystem::remove_all(workspace);
    };

    "runtime_memory_disabled_mirror_does_not_create_snapshot"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-disabled-mirror");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = false, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);
        expect(not std::filesystem::exists(workspace / "MEMORY.md"));

        std::filesystem::remove_all(workspace);
    };

    "runtime_memory_skips_malformed_managed_section"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-invalid-layout");
        const auto snapshot = workspace / "MEMORY.md";

        {
            std::ofstream out(snapshot);
            out << "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n";
        }

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto refresh = runtime.refresh_mirror();
        expect(refresh.skipped);
        expect(read_text_file(snapshot) == "User notes\n<!-- ORANGUTAN:MEMORY:BEGIN -->\nmissing end marker\n");

        std::filesystem::remove_all(workspace);
    };

    "runtime_memory_skips_refresh_when_managed_markers_are_duplicated"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-duplicate-marker");
        const auto snapshot = workspace / "MEMORY.md";

        const std::string original = "User notes\n"
                                     "<!-- ORANGUTAN:MEMORY:BEGIN -->\n"
                                     "old generated block\n"
                                     "<!-- ORANGUTAN:MEMORY:END -->\n"
                                     "stray marker <!-- ORANGUTAN:MEMORY:BEGIN -->\n";
        {
            std::ofstream out(snapshot);
            out << original;
        }

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });
        runtime.remember("project.current", "orangutan memory enhancements", "project", "session:distilled", 0.9);

        const auto refresh = runtime.refresh_mirror();
        expect(refresh.skipped);
        expect(read_text_file(snapshot) == original);

        std::filesystem::remove_all(workspace);
    };

    "runtime_memory_stores_journal_summary_and_appends_daily_journal_file"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-journal");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        RuntimeMemory runtime(store, RuntimeMemoryContext{
                                         .scope = "scope:test",
                                         .workspace = workspace.string(),
                                         .mirror = {.enabled = true, .mirror_file = "MEMORY.md", .journal_dir = "memory"},
                                     });

        const auto result = runtime.store_journal_summary("Reviewed ranking and mirror behavior.");
        expect(result.stored);
        expect(result.mirrored);

        const auto journal_entries = runtime.list("journal", 10);
        expect((journal_entries.size() == 1_ul) >> fatal);
        expect(journal_entries[0].source == "session:journal");
        expect(journal_entries[0].content.find("Reviewed ranking and mirror behavior.") != std::string::npos);

        const auto journal_root = workspace / "memory";
        expect((std::filesystem::exists(journal_root)) >> fatal);
        auto it = std::filesystem::directory_iterator(journal_root);
        expect((it != std::filesystem::directory_iterator{}) >> fatal);
        const auto journal_text = read_text_file(it->path());
        expect(journal_text.find("Reviewed ranking and mirror behavior.") != std::string::npos);

        std::filesystem::remove_all(workspace);
    };

    "search_falls_back_for_non_ascii_queries"_test = [] {
        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
        store.remember("fact.algorithms", "算法高手", "profile");

        const auto results = store.search("算法", {}, 4);
        expect((not results.empty()) >> fatal);
        expect(results.front().key == "fact.algorithms");
    };

    "runtime_memory_mirror_refresh_is_scope_aware"_test = [] {
        const auto workspace = orangutan::testing::unique_test_root("memory-scope-aware");

        MemoryStoreHarness harness;
        MemoryStore store(harness.db_path());
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
        expect(alpha_refresh.refreshed);
        auto snapshot = read_text_file(workspace / "MEMORY.md");
        expect(snapshot.find("alpha memory") != std::string::npos);
        expect(snapshot.find("beta memory") == std::string::npos);

        const auto beta_refresh = beta.refresh_mirror();
        expect(beta_refresh.refreshed);
        snapshot = read_text_file(workspace / "MEMORY.md");
        expect(snapshot.find("beta memory") != std::string::npos);
        expect(snapshot.find("alpha memory") == std::string::npos);

        std::filesystem::remove_all(workspace);
    };
};

boost::ut::suite extract_auto_candidates_suite = [] {
    using namespace boost::ut;

    "chinese_name_stops_at_cjk_comma"_test = [] {
        const auto candidates = memory_detail::extract_auto_candidates("我叫阿明，做编译器");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "profile.name" && candidate.content == "阿明") {
                found = true;
                break;
            }
        }
        expect(found);
    };

    "chinese_remember_skips_fullwidth_colon"_test = [] {
        const auto candidates = memory_detail::extract_auto_candidates("请记住：算法高手。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.category == "fact" && candidate.content == "算法高手") {
                found = true;
                break;
            }
        }
        expect(found);
    };

    "chinese_project_matches_mid_utterance_with_fixed_key"_test = [] {
        const auto candidates = memory_detail::extract_auto_candidates("好的，我们在做编译器。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "project.current" && candidate.category == "project" && candidate.content == "编译器") {
                found = true;
                break;
            }
        }
        expect(found);
    };

    "chinese_remember_matches_mid_utterance_with_hashed_key"_test = [] {
        const auto candidates = memory_detail::extract_auto_candidates("好的，请记住：算法高手。");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.category == "fact" && candidate.content == "算法高手" && candidate.key.rfind("fact.note.", 0) == 0) {
                found = true;
                break;
            }
        }
        expect(found);
    };

    "chinese_name_stops_at_invalid_utf8"_test = [] {
        const auto candidates = memory_detail::extract_auto_candidates(std::string{"我叫阿"} + "\xff" + "明");
        bool found = false;
        for (const auto &candidate : candidates) {
            if (candidate.key == "profile.name" && candidate.content == "阿") {
                found = true;
                break;
            }
        }
        expect(found);
    };
};

boost::ut::suite sanitize_utf8_suite = [] {
    using namespace boost::ut;

    "ascii_passes_through"_test = [] {
        expect(utf8::sanitize("hello world") == std::string{"hello world"});
        expect(utf8::sanitize("") == std::string{""});
    };

    "valid_chinese_passes_through"_test = [] {
        const std::string input = "\xe7\xae\x97\xe6\xb3\x95\xe9\xab\x98\xe6\x89\x8b";
        expect(utf8::sanitize(input) == input);
    };

    "truncated_chinese_character_is_stripped"_test = [] {
        const std::string truncated = "\xe7\xae";
        expect(utf8::sanitize(truncated) == std::string{""});
    };

    "valid_text_followed_by_truncated_character"_test = [] {
        const std::string input = std::string{"hi"} + "\xe4\xb8";
        expect(utf8::sanitize(input) == std::string{"hi"});
    };

    "valid_chinese_followed_by_truncated_character"_test = [] {
        const std::string input = "\xe7\xae\x97\xe6";
        expect(utf8::sanitize(input) == std::string{"\xe7\xae\x97"});
    };

    "accented_latin_passes_through"_test = [] {
        const std::string input = "Jos\xc3\xa9";
        expect(utf8::sanitize(input) == input);
    };

    "truncated_accented_latin_is_stripped"_test = [] {
        const std::string input = "Jos\xc3";
        expect(utf8::sanitize(input) == std::string{"Jos"});
    };

    "emoji_passes_through"_test = [] {
        const std::string input = "\xf0\x9f\x9a\x80";
        expect(utf8::sanitize(input) == input);
    };

    "truncated_emoji_is_stripped"_test = [] {
        const std::string input = "go\xf0\x9f\x9a";
        expect(utf8::sanitize(input) == std::string{"go"});
    };

    "invalid_leading_byte_is_skipped_and_later_text_survives"_test = [] {
        const std::string input = std::string{"ok"} + "\xff" + "再见";
        expect(utf8::sanitize(input) == std::string{"ok再见"});
    };

    "stray_continuation_bytes_are_skipped"_test = [] {
        const std::string input = std::string{"ok"} + "\x80\xbf" + "fine";
        expect(utf8::sanitize(input) == std::string{"okfine"});
    };
};

} // namespace
