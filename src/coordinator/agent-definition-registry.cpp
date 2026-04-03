#include "coordinator/agent-definition-registry.hpp"

#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

namespace orangutan::coordinator {

    namespace {

        std::string trim(const std::string &s) {
            const auto start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) {
                return {};
            }
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(start, end - start + 1);
        }

        std::vector<std::string> split_csv(const std::string &s) {
            std::vector<std::string> result;
            std::istringstream stream(s);
            std::string token;
            while (std::getline(stream, token, ',')) {
                auto trimmed = trim(token);
                if (!trimmed.empty()) {
                    result.push_back(std::move(trimmed));
                }
            }
            return result;
        }

        // Parse a simple YAML-like frontmatter value from a "key: value" line.
        // Returns empty string if the line doesn't match the given key.
        std::string parse_frontmatter_value(const std::string &line, const std::string &key) {
            const auto prefix = key + ":";
            if (line.size() > prefix.size() && line.starts_with(prefix)) {
                return trim(line.substr(prefix.size()));
            }
            return {};
        }

        std::optional<AgentDefinition> parse_agent_file(const std::filesystem::path &path) {
            std::ifstream file(path);
            if (!file.is_open()) {
                spdlog::warn("Could not open agent definition file: {}", path.string());
                return std::nullopt;
            }

            std::string line;

            // Check for frontmatter start
            if (!std::getline(file, line) || trim(line) != "---") {
                spdlog::warn("Agent definition file missing frontmatter: {}", path.string());
                return std::nullopt;
            }

            AgentDefinition def;
            def.key = path.stem().string();
            def.source = "directory";

            // Parse frontmatter
            while (std::getline(file, line)) {
                auto trimmed = trim(line);
                if (trimmed == "---") {
                    break;
                }

                if (auto val = parse_frontmatter_value(trimmed, "description"); !val.empty()) {
                    def.description = val;
                } else if (auto val = parse_frontmatter_value(trimmed, "tools"); !val.empty()) {
                    def.tools = split_csv(val);
                } else if (auto val = parse_frontmatter_value(trimmed, "disallowed_tools"); !val.empty()) {
                    def.disallowed_tools = split_csv(val);
                } else if (auto val = parse_frontmatter_value(trimmed, "model"); !val.empty()) {
                    def.model = val;
                } else if (auto val = parse_frontmatter_value(trimmed, "max_turns"); !val.empty()) {
                    try {
                        def.max_turns = std::stoi(val);
                    } catch (...) {
                        spdlog::warn("Invalid max_turns in {}: {}", path.string(), val);
                    }
                }
            }

            // Rest of file is the prompt addendum
            std::ostringstream body;
            bool first = true;
            while (std::getline(file, line)) {
                if (!first) {
                    body << '\n';
                }
                body << line;
                first = false;
            }
            def.prompt_addendum = trim(body.str());

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

    void AgentDefinitionRegistry::load_from_directory(const std::string &directory_path) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory_path, ec)) {
            spdlog::debug("Agent definition directory does not exist: {}", directory_path);
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
                spdlog::info("Loaded agent definition from file: {} (key={})", entry.path().string(), def->key);
                register_definition(std::move(*def));
            }
        }
    }

    std::optional<AgentDefinition> AgentDefinitionRegistry::find(const std::string &key) const {
        auto it = definitions_.find(key);
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

    bool AgentDefinitionRegistry::has(const std::string &key) const {
        return definitions_.contains(key);
    }

} // namespace orangutan::coordinator
