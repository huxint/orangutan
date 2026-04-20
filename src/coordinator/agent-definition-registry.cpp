#include "coordinator/agent-definition-registry.hpp"

#include "utils/file-io.hpp"
#include "utils/string.hpp"

#include <charconv>
#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::coordinator {

    namespace {

        // Parse a simple YAML-like frontmatter value from a "key: value" line.
        // Returns empty string if the line doesn't match the given key.
        std::string_view parse_frontmatter_value(std::string_view line, std::string_view key) {
            if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == ':') {
                return utils::trim_copy(line.substr(key.size() + 1));
            }
            return {};
        }

        std::optional<AgentDefinition> parse_agent_file(const std::filesystem::path &path) {
            const auto content = fileio::try_read_file(path);
            if (!content.has_value()) {
                spdlog::warn("could not open agent definition file: {}", path.string());
                return std::nullopt;
            }

            const auto lines = utils::split_lines(*content);
            if (lines.empty() || utils::trim_copy(lines.front()) != "---") {
                spdlog::warn("agent definition file missing frontmatter: {}", path.string());
                return std::nullopt;
            }

            AgentDefinition def;
            def.key = path.stem().string();
            def.source = "directory";

            std::size_t index = 1;
            for (; index < lines.size(); ++index) {
                const auto trimmed = utils::trim_copy(lines[index]);
                if (trimmed == "---") {
                    ++index;
                    break;
                }

                if (auto val = parse_frontmatter_value(trimmed, "description"); !val.empty()) {
                    def.description = std::string{val};
                } else if (auto val = parse_frontmatter_value(trimmed, "tools"); !val.empty()) {
                    def.tools = utils::split_trimmed(val);
                } else if (auto val = parse_frontmatter_value(trimmed, "disallowed_tools"); !val.empty()) {
                    def.disallowed_tools = utils::split_trimmed(val);
                } else if (auto val = parse_frontmatter_value(trimmed, "model"); !val.empty()) {
                    def.model = std::string{val};
                } else if (auto val = parse_frontmatter_value(trimmed, "max_turns"); !val.empty()) {
                    int parsed = 0;
                    const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed);
                    if (ec == std::errc{} && ptr == val.data() + val.size()) {
                        def.max_turns = parsed;
                    } else {
                        spdlog::warn("invalid max_turns in {}: {}", path.string(), val);
                    }
                }
            }

            std::string body;
            for (bool first = true; index < lines.size(); ++index) {
                if (!first) {
                    body.push_back('\n');
                }
                body.append(lines[index]);
                first = false;
            }
            def.prompt_addendum = std::string{utils::trim_copy(body)};

            return def;
        }

    } // namespace

    AgentDefinitionRegistry::AgentDefinitionRegistry() = default;

    void AgentDefinitionRegistry::register_definition(AgentDefinition definition) {
        auto key = definition.key;
        definitions_.insert_or_assign(std::move(key), std::move(definition));
    }

    void AgentDefinitionRegistry::load_builtin_definitions() {
        register_definition(AgentDefinition{
            .key = "general-purpose",
            .description = "General-purpose agent for any task",
            .source = "builtin",
        });

        register_definition(AgentDefinition{
            .key = "explorer",
            .description = "Fast exploration agent for codebase research",
            .tools = {"file_read", "grep", "glob", "ls"},
            .source = "builtin",
        });

        register_definition(AgentDefinition{
            .key = "planner",
            .description = "Planning agent for designing implementation strategies",
            .tools = {"file_read", "grep", "glob", "ls"},
            .source = "builtin",
        });
    }

    void AgentDefinitionRegistry::load_from_directory(const std::filesystem::path &directory_path) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory_path, ec)) {
            spdlog::debug("agent definition directory does not exist: {}", directory_path.string());
            return;
        }

        for (const auto &entry : std::filesystem::directory_iterator(directory_path, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().extension() != ".md") {
                continue;
            }

            auto def = parse_agent_file(entry.path());
            if (def.has_value()) {
                spdlog::info("loaded agent definition from file: {} (key={})", entry.path().string(), def->key);
                register_definition(std::move(*def));
            }
        }
    }

    std::optional<AgentDefinition> AgentDefinitionRegistry::find(std::string_view key) const {
        auto it = utils::transparent_find(definitions_, key);
        if (it == definitions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<AgentDefinition> AgentDefinitionRegistry::all() const {
        std::vector<AgentDefinition> result;
        result.reserve(definitions_.size());
        for (const auto &[key, def] : definitions_) {
            result.push_back(def);
        }
        return result;
    }

    bool AgentDefinitionRegistry::has(std::string_view key) const {
        return utils::transparent_contains(definitions_, key);
    }

} // namespace orangutan::coordinator
