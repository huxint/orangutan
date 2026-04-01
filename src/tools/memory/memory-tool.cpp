#include "memory/runtime-memory.hpp"
#include "tools/register.hpp"

#include <spdlog/common.h>
#include <algorithm>
#include "utils/format.hpp"
#include <vector>

namespace orangutan {
    namespace {

        std::string remember_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto key = input.at("key").get<std::string>();
            const auto content = input.at("content").get<std::string>();
            const auto category = input.value("category", std::string{"general"});
            const auto source = input.value("source", std::string{"manual"});
            const auto importance = input.value("importance", 0.5);
            runtime_memory.remember(key, content, category, source, importance);
            return "Stored memory [" + key + "] in category '" + category + "'";
        }

        std::string recall_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto limit = static_cast<std::size_t>(std::max(1, input.value("limit", 8)));
            if (input.contains("category")) {
                const auto category = input.at("category").get<std::string>();
                const auto entries = runtime_memory.recall_by_category(category, limit);
                std::string result;
                for (const auto &[key, content] : entries) {
                    append(result, "[{}] {}\n", key, content);
                }
                return result.empty() ? "(no memories found)" : result;
            }

            const auto query = input.at("query").get<std::string>();
            const auto result = runtime_memory.recall(query, limit);
            return result.empty() ? "(no memories found)" : result;
        }

        std::string forget_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto key = input.at("key").get<std::string>();
            return runtime_memory.forget(key) ? "Forgot memory [" + key + "]" : "No memory found for key [" + key + "]";
        }

        std::string update_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto key = input.at("key").get<std::string>();
            const auto content = input.at("content").get<std::string>();
            const auto category = input.contains("category") ? input.at("category").get<std::string>() : std::string{};
            const auto merge = input.value("merge", true);
            const auto source = input.contains("source") ? input.at("source").get<std::string>() : std::string{};
            const auto importance = input.value("importance", 0.5);
            runtime_memory.update(key, content, category, merge, source, importance);
            return merge ? "Updated memory [" + key + "] with merge" : "Updated memory [" + key + "]";
        }

        std::string format_memory_list(const std::vector<MemoryRecord> &entries) {
            std::string out;
            for (const auto &entry : entries) {
                append(out, "[{}:{}] {}", entry.category, entry.key, entry.content);
                if (!entry.source.empty()) {
                    append(out, " {{source={}}}", entry.source);
                }
                out.push_back('\n');
            }
            return out;
        }

        std::string list_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto category = input.value("category", std::string{});
            const auto limit = static_cast<std::size_t>(std::max(1, input.value("limit", 20)));
            const auto entries = runtime_memory.list(category, limit);
            const auto result = format_memory_list(entries);
            return result.empty() ? "(no memories found)" : result;
        }

        std::string stats_memory(RuntimeMemory &runtime_memory) {
            const auto stats = runtime_memory.stats();
            return spdlog::fmt_lib::format("total={}\ncategories={}\nmanual={}\nauto={}\njournal={}", stats.total, stats.categories, stats.manual_entries, stats.auto_entries,
                                           stats.journal_entries);
        }

    } // namespace

    void register_builtin_memory_tools(ToolRegistry &registry, RuntimeMemory &runtime_memory) {
        registry.register_tool({.definition = {.name = "remember",
                                               .description = "Store a fact or preference for future conversations.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"key", {{"type", "string"}, {"description", "Stable lookup key for the memory"}}},
                                                                  {"content", {{"type", "string"}, {"description", "The memory value to store"}}},
                                                                  {"category", {{"type", "string"}, {"description", "Optional category label"}}},
                                                                  {"source", {{"type", "string"}, {"description", "Optional memory source label"}}},
                                                                  {"importance", {{"type", "number"}, {"description", "Optional importance score from 0 to 1"}}}}},
                                                                {"required", nlohmann::json::array({"key", "content"})}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return remember_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "recall",
                                               .description = "Recall stored memories by free-text query or category.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"query", {{"type", "string"}, {"description", "Search text to match against memory keys or content"}}},
                                                                  {"category", {{"type", "string"}, {"description", "Return all memories in a category"}}},
                                                                  {"limit", {{"type", "integer"}, {"description", "Maximum number of memories to return"}}}}},
                                                                {"anyOf", nlohmann::json::array({
                                                                              nlohmann::json{{"required", nlohmann::json::array({"query"})}},
                                                                              nlohmann::json{{"required", nlohmann::json::array({"category"})}},
                                                                          })}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return recall_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "forget",
                                               .description = "Delete a stored memory by key.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties", {{"key", {{"type", "string"}, {"description", "The memory key to delete"}}}}},
                                                                {"required", nlohmann::json::array({"key"})}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return forget_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_store",
                                               .description = "Plugin-style alias for remember.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"key", {{"type", "string"}, {"description", "Stable lookup key for the memory"}}},
                                                                  {"content", {{"type", "string"}, {"description", "The memory value to store"}}},
                                                                  {"category", {{"type", "string"}, {"description", "Optional category label"}}},
                                                                  {"source", {{"type", "string"}, {"description", "Optional memory source label"}}},
                                                                  {"importance", {{"type", "number"}, {"description", "Optional importance score from 0 to 1"}}}}},
                                                                {"required", nlohmann::json::array({"key", "content"})}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return remember_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_recall",
                                               .description = "Plugin-style alias for recall.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"query", {{"type", "string"}, {"description", "Search text to match against memory keys or content"}}},
                                                                  {"category", {{"type", "string"}, {"description", "Return all memories in a category"}}},
                                                                  {"limit", {{"type", "integer"}, {"description", "Maximum number of memories to return"}}}}},
                                                                {"anyOf", nlohmann::json::array({
                                                                              nlohmann::json{{"required", nlohmann::json::array({"query"})}},
                                                                              nlohmann::json{{"required", nlohmann::json::array({"category"})}},
                                                                          })}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return recall_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_forget",
                                               .description = "Plugin-style alias for forget.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties", {{"key", {{"type", "string"}, {"description", "The memory key to delete"}}}}},
                                                                {"required", nlohmann::json::array({"key"})}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return forget_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_update",
                                               .description = "Update or merge a memory entry.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"key", {{"type", "string"}, {"description", "Stable lookup key for the memory"}}},
                                                                  {"content", {{"type", "string"}, {"description", "The memory value to store"}}},
                                                                  {"category", {{"type", "string"}, {"description", "Optional category label"}}},
                                                                  {"merge", {{"type", "boolean"}, {"description", "Merge with existing content instead of replacing it"}}},
                                                                  {"source", {{"type", "string"}, {"description", "Optional memory source label"}}},
                                                                  {"importance", {{"type", "number"}, {"description", "Optional importance score from 0 to 1"}}}}},
                                                                {"required", nlohmann::json::array({"key", "content"})}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return update_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_list",
                                               .description = "List recent memories, optionally filtered by category.",
                                               .input_schema = {{"type", "object"},
                                                                {"properties",
                                                                 {{"category", {{"type", "string"}, {"description", "Optional category filter"}}},
                                                                  {"limit", {{"type", "integer"}, {"description", "Maximum number of memories to return"}}}}},
                                                                {"required", nlohmann::json::array()}}},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return list_memory(input, runtime_memory);
                                }});

        registry.register_tool({.definition = {.name = "memory_stats",
                                               .description = "Return summary statistics for the current memory scope.",
                                               .input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}, {"required", nlohmann::json::array()}}},
                                .execute = [&runtime_memory](const nlohmann::json & /*input*/) {
                                    return stats_memory(runtime_memory);
                                }});
    }

} // namespace orangutan
