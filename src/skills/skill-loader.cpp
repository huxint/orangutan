#include "skills/skill-loader.hpp"
#include "utils/file-io.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <spdlog/spdlog.h>
#include "utils/format.hpp"
#include <unordered_map>

namespace orangutan::skills {

    namespace {

        constexpr std::string_view yaml_delimiter = "---";

        struct ParsedSkillFile {
            bool valid = false;
            SkillDef skill;
        };

        std::string_view strip_cr(std::string_view line) {
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            return line;
        }

        bool is_yaml_delimiter(std::string_view line) {
            return strip_cr(line) == yaml_delimiter;
        }

        bool is_blank_line(std::string_view line) {
            return std::ranges::all_of(strip_cr(line), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
        }

        std::vector<std::string> split_lines(const std::string &content) {
            std::istringstream stream(content);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(stream, line)) {
                lines.push_back(line);
            }
            return lines;
        }

        void strip_utf8_bom(std::string &line) {
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (line.starts_with(bom)) {
                line.erase(0, bom.size());
            }
        }

        bool env_vars_satisfied(const std::vector<std::string> &env) {
            return std::ranges::all_of(env, [](const std::string &var) {
                if (std::getenv(var.c_str()) == nullptr) {
                    spdlog::debug("Skill env dependency not met: {} is not set", var);
                    return false;
                }
                return true;
            });
        }

        // Trim leading/trailing whitespace from a string_view
        std::string_view trim(std::string_view sv) {
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())) != 0) {
                sv.remove_prefix(1);
            }
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())) != 0) {
                sv.remove_suffix(1);
            }
            return sv;
        }

        // Strip surrounding quotes (single or double) from a YAML value
        std::string strip_quotes(std::string_view sv) {
            if (sv.size() >= 2) {
                if ((sv.front() == '"' && sv.back() == '"') || (sv.front() == '\'' && sv.back() == '\'')) {
                    sv.remove_prefix(1);
                    sv.remove_suffix(1);
                }
            }
            return std::string(sv);
        }

        // Parse simple YAML frontmatter: key: value, key: [a, b, c]
        SkillDef parse_yaml_frontmatter(const std::string &yaml_text, const std::string &source_path) {
            SkillDef skill;
            skill.source_path = source_path;

            auto lines = split_lines(yaml_text);
            for (const auto &line : lines) {
                auto trimmed = trim(line);
                if (trimmed.empty() || trimmed.front() == '#') {
                    continue;
                }

                auto colon_pos = trimmed.find(':');
                if (colon_pos == std::string_view::npos) {
                    continue;
                }

                auto key = trim(trimmed.substr(0, colon_pos));
                auto raw_value = trim(trimmed.substr(colon_pos + 1));

                // Check for inline YAML array: [item1, item2]
                auto parse_yaml_array = [](std::string_view val) -> std::vector<std::string> {
                    std::vector<std::string> result;
                    if (val.size() >= 2 && val.front() == '[' && val.back() == ']') {
                        val.remove_prefix(1);
                        val.remove_suffix(1);
                        auto str = std::string(val);
                        std::istringstream items{str};
                        std::string item;
                        while (std::getline(items, item, ',')) {
                            auto t = trim(item);
                            if (!t.empty()) {
                                result.push_back(strip_quotes(t));
                            }
                        }
                    }
                    return result;
                };

                if (key == "name") {
                    skill.name = strip_quotes(raw_value);
                } else if (key == "description") {
                    skill.description = strip_quotes(raw_value);
                } else if (key == "tools") {
                    skill.tools = parse_yaml_array(raw_value);
                } else if (key == "env") {
                    skill.env = parse_yaml_array(raw_value);
                }
            }

            return skill;
        }

        ParsedSkillFile parse_skill_file(const std::string &content, const std::string &source_path) {
            auto lines = split_lines(content);
            if (lines.empty()) {
                spdlog::warn("Skill file has no frontmatter: {}", source_path);
                return {};
            }

            strip_utf8_bom(lines.front());

            std::size_t open_index = 0;
            while (open_index < lines.size() && is_blank_line(lines[open_index])) {
                ++open_index;
            }

            if (open_index == lines.size()) {
                spdlog::warn("Skill file has no frontmatter: {}", source_path);
                return {};
            }

            if (!is_yaml_delimiter(lines[open_index])) {
                spdlog::warn("Skill file has no frontmatter: {}", source_path);
                return {};
            }

            // Extract frontmatter content and body
            std::string fm_buf;
            bool found_closing = false;
            std::size_t body_start_index = lines.size();

            // YAML: simple close on first ---
            for (std::size_t index = open_index + 1; index < lines.size(); ++index) {
                if (is_yaml_delimiter(lines[index])) {
                    found_closing = true;
                    body_start_index = index + 1;
                    break;
                }
                fm_buf.append(lines[index]);
                fm_buf.push_back('\n');
            }

            if (!found_closing) {
                spdlog::warn("Skill file has unclosed frontmatter: {}", source_path);
                return {};
            }

            // Build body
            std::string body;
            for (std::size_t index = body_start_index; index < lines.size(); ++index) {
                if (index != body_start_index) {
                    body.push_back('\n');
                }
                body.append(lines[index]);
            }
            auto fm_text = fm_buf;

            SkillDef skill;

            skill = parse_yaml_frontmatter(fm_text, source_path);
            skill.body = body;
            if (skill.name.empty() || skill.description.empty()) {
                spdlog::warn("Skill file missing required 'name' or 'description': {}", source_path);
                return {};
            }

            return {.valid = true, .skill = std::move(skill)};
        }

    } // namespace

    std::vector<std::string> resolve_skill_directories(const std::vector<std::string> &configured_skill_paths, const std::string &workspace_root) {
        if (!configured_skill_paths.empty()) {
            return configured_skill_paths;
        }

        std::vector<std::string> directories;
        if (const char *home = std::getenv("HOME")) {
            directories.emplace_back(std::string(home) + "/.orangutan/skills");
        }
        if (!workspace_root.empty()) {
            directories.emplace_back(workspace_root + "/.orangutan/skills");
        }
        return directories;
    }

    void SkillLoader::load_from_directories(const std::vector<std::string> &directories) {
        // Use a map for same-name shadowing: later directories override earlier ones
        std::unordered_map<std::string, SkillDef> by_name;

        for (const auto &dir_path : directories) {
            std::error_code ec;
            if (!std::filesystem::exists(dir_path, ec) || ec || !std::filesystem::is_directory(dir_path, ec) || ec) {
                spdlog::debug("Skill directory does not exist, skipping: {}", dir_path);
                continue;
            }

            std::vector<std::filesystem::directory_entry> entries;
            for (std::filesystem::directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec)) {
                const auto &entry = *it;
                if (!entry.is_directory(ec) || ec) {
                    continue;
                }
                entries.push_back(entry);
            }

            if (ec) {
                spdlog::warn("Error scanning skill directory {}: {}", dir_path, ec.message());
                continue;
            }

            std::ranges::sort(entries, [](const std::filesystem::directory_entry &left, const std::filesystem::directory_entry &right) {
                return left.path().lexically_normal().string() < right.path().lexically_normal().string();
            });

            for (const auto &entry : entries) {

                auto skill_file = entry.path() / "SKILL.md";
                if (!std::filesystem::exists(skill_file, ec) || ec) {
                    continue;
                }

                auto content = fileio::try_read_file(skill_file).value_or(std::string{});
                if (content.empty()) {
                    continue;
                }

                auto parsed = parse_skill_file(content, skill_file.string());
                if (!parsed.valid) {
                    continue;
                }

                if (!env_vars_satisfied(parsed.skill.env)) {
                    spdlog::debug("Skipping skill '{}' — unmet env dependencies", parsed.skill.name);
                    continue;
                }

                spdlog::debug("Loaded skill '{}' from {}", parsed.skill.name, parsed.skill.source_path);
                by_name.insert_or_assign(parsed.skill.name, std::move(parsed.skill));
            }
        }

        skills_.clear();
        skills_.reserve(by_name.size());
        for (auto &[name, skill] : by_name) {
            skills_.push_back(std::move(skill));
        }
        std::ranges::sort(skills_, {}, &SkillDef::name);
    }

    std::string SkillLoader::build_prompt_section() const {
        if (skills_.empty()) {
            return {};
        }

        std::string out;
        out.append("\n\n## Active Skills\n");
        for (const auto &skill : skills_) {
            append(out, "\n### {}\n", skill.name);
            out.append(skill.body);
        }
        return out;
    }

} // namespace orangutan::skills
