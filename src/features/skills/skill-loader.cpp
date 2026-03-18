#include "features/skills/skill-loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <toml++/toml.hpp>
#include <unordered_map>

namespace orangutan {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view frontmatter_delimiter = "+++";

struct ParsedSkillFile {
    bool valid = false;
    SkillDef skill;
};

std::string read_file(const fs::path &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

bool is_delimiter_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line == frontmatter_delimiter;
}

bool is_blank_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return std::ranges::all_of(line, [](unsigned char ch) {
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

ParsedSkillFile parse_skill_file(const std::string &content, const std::string &source_path) {
    auto lines = split_lines(content);
    if (lines.empty()) {
        spdlog::warn("Skill file has no TOML frontmatter: {}", source_path);
        return {};
    }

    strip_utf8_bom(lines.front());

    size_t open_index = 0;
    while (open_index < lines.size() && is_blank_line(lines[open_index])) {
        ++open_index;
    }

    if (open_index == lines.size() || !is_delimiter_line(lines[open_index])) {
        spdlog::warn("Skill file has no TOML frontmatter: {}", source_path);
        return {};
    }

    std::ostringstream toml_buf;
    bool found_closing_delimiter = false;
    size_t body_start_index = lines.size();
    for (size_t index = open_index + 1; index < lines.size(); ++index) {
        if (is_delimiter_line(lines[index])) {
            try {
                auto parsed_frontmatter = toml::parse(toml_buf.str());
                (void)parsed_frontmatter;
                found_closing_delimiter = true;
                body_start_index = index + 1;
                break;
            } catch (const toml::parse_error &) {
                continue;
            }
        }
        toml_buf << lines[index] << '\n';
    }

    if (!found_closing_delimiter) {
        spdlog::warn("Skill file has unclosed TOML frontmatter: {}", source_path);
        return {};
    }

    std::ostringstream body_buf;
    for (size_t index = body_start_index; index < lines.size(); ++index) {
        if (index != body_start_index) {
            body_buf << '\n';
        }
        body_buf << lines[index];
    }
    auto toml_text = toml_buf.str();
    auto body = body_buf.str();

    // Parse TOML frontmatter
    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (const toml::parse_error &e) {
        spdlog::warn("Invalid TOML frontmatter in {}: {}", source_path, e.what());
        return {};
    }

    auto name = tbl["name"].value<std::string>();
    auto description = tbl["description"].value<std::string>();
    if (!name.has_value() || !description.has_value()) {
        spdlog::warn("Skill file missing required 'name' or 'description': {}", source_path);
        return {};
    }

    SkillDef skill;
    skill.name = *name;
    skill.description = *description;
    skill.body = body;
    skill.source_path = source_path;

    if (const auto *tools_arr = tbl["tools"].as_array()) {
        for (const auto &item : *tools_arr) {
            if (auto s = item.value<std::string>()) {
                skill.tools.push_back(*s);
            }
        }
    }
    if (const auto *env_arr = tbl["env"].as_array()) {
        for (const auto &item : *env_arr) {
            if (auto s = item.value<std::string>()) {
                skill.env.push_back(*s);
            }
        }
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
        if (!fs::exists(dir_path, ec) || ec || !fs::is_directory(dir_path, ec) || ec) {
            spdlog::debug("Skill directory does not exist, skipping: {}", dir_path);
            continue;
        }

        std::vector<fs::directory_entry> entries;
        for (fs::directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec)) {
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

        std::ranges::sort(entries, [](const fs::directory_entry &left, const fs::directory_entry &right) {
            return left.path().lexically_normal().string() < right.path().lexically_normal().string();
        });

        for (const auto &entry : entries) {

            auto skill_file = entry.path() / "SKILL.md";
            if (!fs::exists(skill_file, ec) || ec) {
                continue;
            }

            auto content = read_file(skill_file);
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

    std::ostringstream out;
    out << "\n\n## Active Skills\n";
    for (const auto &skill : skills_) {
        out << "\n### " << skill.name << "\n";
        out << skill.body;
    }
    return out.str();
}

} // namespace orangutan
