#include "memory/runtime-memory.hpp"
#include "memory/memory-type.hpp"
#include "tools/register.hpp"
#include "types/base.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <stdexcept>

namespace orangutan::tools {
    namespace {

        [[nodiscard]]
        memory_type parse_memory_kind(const nlohmann::json &input) {
            const auto kind = input.value("kind", std::string{"user"});
            auto parsed = magic_enum::enum_cast<memory_type>(kind, magic_enum::case_insensitive);
            if (!parsed.has_value()) {
                throw std::runtime_error("memory kind must be one of: user, feedback, project, reference");
            }
            return *parsed;
        }

        [[nodiscard]]
        std::size_t parse_limit(const nlohmann::json &input, int default_limit) {
            return static_cast<std::size_t>(std::max(1, input.value("limit", default_limit)));
        }

        std::string remember_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto key = input.at("key").get<std::string>();
            const auto content = input.at("content").get<std::string>();
            if (utils::trim_copy(key).empty() || utils::trim_copy(content).empty()) {
                throw std::runtime_error("remember expects non-empty 'key' and 'content'");
            }
            const auto kind = parse_memory_kind(input);
            runtime_memory.remember(key, content, kind);
            return "Remembered [" + key + "] as " + std::string(magic_enum::enum_name(kind)) + " memory.";
        }

        std::string recall_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto query = input.at("query").get<std::string>();
            const auto result = runtime_memory.recall(query, parse_limit(input, 8));
            return result.empty() ? "(no memories found)" : result;
        }

        std::string forget_memory(const nlohmann::json &input, RuntimeMemory &runtime_memory) {
            const auto key = input.at("key").get<std::string>();
            return runtime_memory.forget(key) ? "Forgot memory [" + key + "]" : "No memory found for key [" + key + "]";
        }

        [[nodiscard]]
        nlohmann::json remember_schema() {
            return {
                {"type", "object"},
                {"additionalProperties", false},
                {"properties",
                 {
                     {"key", {{"type", "string"}, {"description", "Stable lookup key, such as preference.reply-style or project.current"}}},
                     {"content", {{"type", "string"}, {"minLength", 1}, {"description", "Concise memory text to use as future context"}}},
                     {"kind",
                      {{"type", "string"},
                       {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})},
                       {"description", "Memory kind: user, feedback, project, or reference"}}},
                 }},
                {"required", nlohmann::json::array({"key", "content"})},
            };
        }

        [[nodiscard]]
        nlohmann::json recall_schema() {
            return {
                {"type", "object"},
                {"additionalProperties", false},
                {"properties",
                 {
                     {"query", {{"type", "string"}, {"description", "Search text for relevant remembered facts"}}},
                     {"limit", {{"type", "integer"}, {"description", "Maximum number of memories to return"}}},
                 }},
                {"required", nlohmann::json::array({"query"})},
            };
        }

        [[nodiscard]]
        nlohmann::json forget_schema() {
            return {
                {"type", "object"},
                {"additionalProperties", false},
                {"properties", {{"key", {{"type", "string"}, {"description", "Stable key of the memory to delete"}}}}},
                {"required", nlohmann::json::array({"key"})},
            };
        }

    } // namespace

    void register_builtin_memory_tools(ToolRegistry &registry, RuntimeMemory &runtime_memory) {
        registry.register_tool({.definition = {.name = "remember",
                                               .description = "Store a durable user preference, project fact, correction, or reference for future turns.",
                                               .input_schema = remember_schema()},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return remember_memory(input, runtime_memory);
                                },
                                .deferred = true});

        registry.register_tool({.definition = {.name = "recall",
                                               .description = "Search long-term memory when the user asks about prior context or when extra remembered context would help.",
                                               .input_schema = recall_schema()},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return recall_memory(input, runtime_memory);
                                },
                                .deferred = true});

        registry.register_tool({.definition = {.name = "forget",
                                               .description = "Delete one remembered item by key when it is wrong, stale, or no longer wanted.",
                                               .input_schema = forget_schema()},
                                .execute = [&runtime_memory](const nlohmann::json &input) {
                                    return forget_memory(input, runtime_memory);
                                },
                                .deferred = true});
    }

} // namespace orangutan::tools
