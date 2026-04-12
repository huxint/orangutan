#pragma once

#include "permissions/rule-parser.hpp"
#include "utils/file-io.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

namespace orangutan::skills {

    enum class skill_source : base::u8 {
        managed,
        user,
        project,
        workspace,
        bundled,
        plugin,
    };

    enum class skill_scope : base::u8 {
        always,
        conditional,
        manual_only,
    };

    enum class skill_call_origin : base::u8 {
        manual,
        automatic,
    };

    enum class skill_invoke_status : base::u8 {
        ok,
        not_found,
        invalid_args,
        blocked,
        runtime_error,
    };

    struct skill_diagnostic {
        std::string code;
        std::string message;
        std::string source_path;
    };

    struct skill_activation_condition {
        std::vector<std::string> paths_any;
        std::vector<std::string> env_all;
        std::vector<std::string> platforms_any;
    };

    struct skill_view {
        std::string id;
        std::string name;
        std::string description;
        std::string body;
        skill_source source = skill_source::workspace;
        skill_scope scope = skill_scope::always;
        bool active = true;
        std::string source_path;
        std::vector<std::string> tools;
        std::vector<skill_diagnostic> diagnostics;
        skill_activation_condition activation;
    };

    struct skill_catalog_view {
        std::vector<skill_view> skills;
    };

    [[nodiscard]]
    inline std::string render_skill_prompt_section(const skill_catalog_view &catalog) {
        if (catalog.skills.empty()) {
            return {};
        }

        std::vector<const skill_view *> ordered;
        ordered.reserve(catalog.skills.size());
        for (const auto &skill : catalog.skills) {
            ordered.push_back(&skill);
        }
        std::ranges::sort(ordered, [](const skill_view *left, const skill_view *right) {
            return left->name < right->name;
        });

        std::string out;
        out.append("\n\n## Available Skills\n");
        out.append("Use the `skill` tool with a skill name to load its full instructions before using it.\n");
        for (const auto *skill : ordered) {
            out.append("\n- **");
            out.append(skill->name);
            out.append("**: ");
            out.append(skill->description);
        }
        return out;
    }

    struct skill_runtime_config {
        std::vector<std::filesystem::path> directories;
        std::filesystem::path workspace_root;
        skill_source source = skill_source::workspace;
    };

    struct skill_list_query {
        bool include_inactive = true;
    };

    struct skill_invoke_request {
        std::string skill_id;
        std::string name;
        skill_call_origin call_origin = skill_call_origin::automatic;
    };

    struct skill_invoke_result {
        skill_invoke_status status = skill_invoke_status::runtime_error;
        std::string content;
        std::optional<skill_view> resolved_skill;
        std::vector<skill_diagnostic> diagnostics;
    };

    class skill_runtime {
    public:
        void set_workspace_root(const std::filesystem::path &workspace_root) {
            config_.workspace_root = workspace_root;
        }

        void load_from_directories(const std::vector<std::filesystem::path> &directories) {
            auto next_config = config_;
            next_config.directories = directories;
            reload(next_config);
        }

        void reload(const skill_runtime_config &config) {
            config_ = config;
            auto next = std::make_shared<catalog_snapshot>();
            next->workspace_root = config.workspace_root;

            for (const auto &dir_path : config.directories) {
                std::error_code ec;
                if (!std::filesystem::exists(dir_path, ec) || ec || !std::filesystem::is_directory(dir_path, ec) || ec) {
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

                std::ranges::sort(entries, [](const std::filesystem::directory_entry &left, const std::filesystem::directory_entry &right) {
                    return left.path().lexically_normal().string() < right.path().lexically_normal().string();
                });

                for (const auto &entry : entries) {
                    const auto skill_file = entry.path() / "SKILL.md";
                    const auto content = fileio::try_read_file(skill_file).value_or(std::string{});
                    if (content.empty()) {
                        continue;
                    }

                    auto parsed = parse_skill_file(content, skill_file.string(), config.source, config.workspace_root);
                    if (!parsed.has_value()) {
                        next->diagnostics.push_back(parsed.error());
                        continue;
                    }

                    auto skill = std::move(parsed).value();
                    if (!conditions_valid(skill, next->diagnostics)) {
                        continue;
                    }
                    next->loaded.push_back(std::move(skill));
                }
            }

            publish_deduplicated(*next);
            const std::vector<std::filesystem::path> empty_paths;
            for (auto &skill : next->skills) {
                if (skill.scope != skill_scope::conditional) {
                    continue;
                }
                skill.active = evaluate_paths_any(skill.activation.paths_any, empty_paths, next->workspace_root) && evaluate_env_all(skill.activation.env_all) &&
                               evaluate_platforms_any(skill.activation.platforms_any);
            }
            snapshot_ = std::move(next);
            probe_signature_ = build_probe_signature(config_);
        }

        void activate_for_paths(const std::vector<std::filesystem::path> &paths) {
            auto current = snapshot_;
            if (!current) {
                return;
            }

            auto next = std::make_shared<catalog_snapshot>(*current);
            for (auto &skill : next->skills) {
                if (skill.scope != skill_scope::conditional) {
                    continue;
                }
                skill.active = evaluate_paths_any(skill.activation.paths_any, paths, next->workspace_root) && evaluate_env_all(skill.activation.env_all) &&
                               evaluate_platforms_any(skill.activation.platforms_any);
            }
            snapshot_ = std::move(next);
        }

        [[nodiscard]]
        skill_catalog_view list(const skill_list_query &query) const {
            probe_reload_if_needed();

            skill_catalog_view out;
            const auto current = snapshot_;
            if (!current) {
                return out;
            }

            for (const auto &skill : current->skills) {
                if (!query.include_inactive && !skill.active) {
                    continue;
                }
                out.skills.push_back(skill);
            }
            return out;
        }

        [[nodiscard]]
        const skill_view *find_by_id(std::string_view skill_id) const {
            const auto current = snapshot_;
            if (!current) {
                return nullptr;
            }
            auto it = std::ranges::find_if(current->skills, [skill_id](const skill_view &skill) {
                return skill.id == skill_id;
            });
            return it == current->skills.end() ? nullptr : &(*it);
        }

        [[nodiscard]]
        const skill_view *find_by_name(std::string_view name) const {
            const auto current = snapshot_;
            if (!current) {
                return nullptr;
            }
            auto it = std::ranges::find_if(current->skills, [name](const skill_view &skill) {
                return skill.name == name;
            });
            return it == current->skills.end() ? nullptr : &(*it);
        }

        [[nodiscard]]
        skill_invoke_result invoke(const skill_invoke_request &request) const {
            probe_reload_if_needed();

            skill_invoke_result result;
            const auto current = snapshot_;
            if (!current) {
                result.status = skill_invoke_status::runtime_error;
                result.diagnostics.push_back(skill_diagnostic{.code = "runtime_not_loaded", .message = "skill runtime is not loaded"});
                return result;
            }

            const skill_view *skill = nullptr;
            if (!request.skill_id.empty()) {
                skill = find_by_id(request.skill_id);
            }
            if (skill == nullptr && !request.name.empty()) {
                skill = find_by_name(request.name);
            }
            if (skill == nullptr) {
                result.status = skill_invoke_status::not_found;
                result.diagnostics.push_back(skill_diagnostic{.code = "skill_not_found", .message = "skill not found"});
                return result;
            }

            if (skill->scope == skill_scope::manual_only && request.call_origin == skill_call_origin::automatic) {
                result.status = skill_invoke_status::blocked;
                result.diagnostics.push_back(
                    skill_diagnostic{.code = "blocked_manual_only", .message = "skill is manual_only and cannot be auto-invoked", .source_path = skill->source_path});
                result.resolved_skill = *skill;
                return result;
            }

            if (skill->scope == skill_scope::conditional && !skill->active) {
                result.status = skill_invoke_status::blocked;
                result.diagnostics.push_back(
                    skill_diagnostic{.code = "blocked_inactive_conditional", .message = "conditional skill is inactive for current context", .source_path = skill->source_path});
                result.resolved_skill = *skill;
                return result;
            }

            result.status = skill_invoke_status::ok;
            result.content = "# Skill: " + skill->name + "\n\n" + skill->body;
            result.resolved_skill = *skill;
            return result;
        }

        [[nodiscard]]
        const std::vector<skill_diagnostic> &diagnostics() const {
            static const std::vector<skill_diagnostic> EMPTY;
            const auto current = snapshot_;
            return current ? current->diagnostics : EMPTY;
        }

    private:
        struct catalog_snapshot {
            std::filesystem::path workspace_root;
            std::vector<skill_view> loaded;
            std::vector<skill_view> skills;
            std::vector<skill_diagnostic> diagnostics;
        };

        skill_runtime_config config_;
        std::shared_ptr<catalog_snapshot> snapshot_;
        std::string probe_signature_;

        [[nodiscard]]
        static std::string build_probe_signature(const skill_runtime_config &config) {
            std::string signature;
            signature.reserve(config.directories.size() * 64UL);

            signature.append(std::to_string(std::to_underlying(config.source)));
            signature.push_back('|');
            signature.append(config.workspace_root.lexically_normal().generic_string());
            signature.push_back('|');

            for (const auto &dir_path : config.directories) {
                const auto normalized_dir = dir_path.lexically_normal();
                signature.append(normalized_dir.generic_string());
                signature.push_back(':');

                std::error_code ec;
                const auto exists = std::filesystem::exists(normalized_dir, ec);
                if (ec || !exists) {
                    signature.append("missing;");
                    continue;
                }

                std::vector<std::string> entries;
                for (std::filesystem::directory_iterator it(normalized_dir, ec), end; !ec && it != end; it.increment(ec)) {
                    const auto &entry = *it;
                    if (!entry.is_directory(ec) || ec) {
                        continue;
                    }

                    const auto skill_file = entry.path() / "SKILL.md";
                    std::error_code skill_ec;
                    const auto skill_exists = std::filesystem::exists(skill_file, skill_ec);

                    std::string entry_signature = entry.path().lexically_normal().generic_string();
                    entry_signature.push_back('@');
                    if (skill_ec || !skill_exists) {
                        entry_signature.append("missing");
                    } else {
                        const auto stamp = std::filesystem::last_write_time(skill_file, skill_ec);
                        if (skill_ec) {
                            entry_signature.append("error");
                        } else {
                            entry_signature.append(std::to_string(stamp.time_since_epoch().count()));
                        }
                    }
                    entries.push_back(std::move(entry_signature));
                }

                std::ranges::sort(entries);
                for (const auto &entry : entries) {
                    signature.append(entry);
                    signature.push_back(',');
                }
                signature.push_back(';');
            }

            return signature;
        }

        void probe_reload_if_needed() const {
            if (!snapshot_) {
                return;
            }

            const auto current_signature = build_probe_signature(config_);
            if (current_signature == probe_signature_) {
                return;
            }

            auto *self = const_cast<skill_runtime *>(this);
            self->reload(self->config_);
        }

        [[nodiscard]]
        static std::string make_skill_id(skill_source source, const std::filesystem::path &source_path, std::string_view skill_name) {
            const auto source_name = magic_enum::enum_name(source);
            const auto source_label = source_name.empty() ? std::string_view{"workspace"} : source_name;
            auto canonical = source_path.lexically_normal().string();
            return std::string(source_label) + ":" + canonical + "#" + std::string(skill_name);
        }

        [[nodiscard]]
        static std::string strip_quotes(std::string_view sv) {
            if (sv.size() >= 2) {
                if ((sv.front() == '"' && sv.back() == '"') || (sv.front() == '\'' && sv.back() == '\'')) {
                    sv.remove_prefix(1);
                    sv.remove_suffix(1);
                }
            }
            return std::string(sv);
        }

        [[nodiscard]]
        static std::vector<std::string> parse_yaml_array(std::string_view value) {
            std::vector<std::string> out;
            auto trimmed = utils::trim_copy(value);
            if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                trimmed.remove_prefix(1);
                trimmed.remove_suffix(1);
                auto items = split_trimmed(trimmed);
                out.reserve(items.size());
                for (const auto &item : items) {
                    out.push_back(strip_quotes(item));
                }
            }
            return out;
        }

        [[nodiscard]]
        static std::expected<skill_view, skill_diagnostic> parse_skill_file(const std::string &content, const std::string &source_path, skill_source source,
                                                                            const std::filesystem::path &workspace_root) {
            std::vector<std::string> lines;
            {
                std::istringstream stream(content);
                std::string line;
                while (std::getline(stream, line)) {
                    lines.push_back(line);
                }
            }

            if (lines.empty()) {
                return std::unexpected(skill_diagnostic{.code = "missing_frontmatter", .message = "skill file has no frontmatter", .source_path = source_path});
            }

            if (lines[0].starts_with("\xEF\xBB\xBF")) {
                lines[0].erase(0, 3);
            }

            std::size_t open_index = 0;
            while (open_index < lines.size() && utils::trim_copy(lines[open_index]).empty()) {
                ++open_index;
            }
            if (open_index == lines.size() || utils::trim_copy(lines[open_index]) != "---") {
                return std::unexpected(skill_diagnostic{.code = "missing_frontmatter", .message = "skill file has no yaml frontmatter", .source_path = source_path});
            }

            std::size_t close_index = open_index + 1;
            while (close_index < lines.size() && utils::trim_copy(lines[close_index]) != "---") {
                ++close_index;
            }
            if (close_index == lines.size()) {
                return std::unexpected(skill_diagnostic{.code = "unclosed_frontmatter", .message = "skill file has unclosed yaml frontmatter", .source_path = source_path});
            }

            skill_view skill;
            skill.source = source;
            skill.source_path = source_path;
            skill.scope = skill_scope::always;
            skill.active = true;
            auto declared_scope = skill_scope::always;

            for (std::size_t i = open_index + 1; i < close_index; ++i) {
                auto line = utils::trim_copy(lines[i]);
                if (line.empty() || line.starts_with("#")) {
                    continue;
                }
                const auto colon = line.find(':');
                if (colon == std::string_view::npos) {
                    continue;
                }
                const auto key = utils::trim_copy(line.substr(0, colon));
                const auto value = utils::trim_copy(line.substr(colon + 1));

                if (key == "name") {
                    skill.name = strip_quotes(value);
                } else if (key == "description") {
                    skill.description = strip_quotes(value);
                } else if (key == "tools") {
                    skill.tools = parse_yaml_array(value);
                } else if (key == "scope") {
                    const auto scope_text = ascii_to_lower_copy(strip_quotes(value));
                    if (scope_text == "manual_only") {
                        declared_scope = skill_scope::manual_only;
                    } else if (scope_text == "conditional") {
                        declared_scope = skill_scope::conditional;
                    } else {
                        declared_scope = skill_scope::always;
                    }
                } else if (key == "paths_any") {
                    skill.activation.paths_any = parse_yaml_array(value);
                } else if (key == "env_all") {
                    skill.activation.env_all = parse_yaml_array(value);
                } else if (key == "env") {
                    skill.activation.env_all = parse_yaml_array(value);
                } else if (key == "platforms_any") {
                    skill.activation.platforms_any = parse_yaml_array(value);
                }
            }

            const auto has_conditions = !skill.activation.paths_any.empty() || !skill.activation.env_all.empty() || !skill.activation.platforms_any.empty();
            if (declared_scope == skill_scope::manual_only) {
                skill.scope = skill_scope::manual_only;
                skill.active = true;
            } else if (declared_scope == skill_scope::conditional || has_conditions) {
                if (has_conditions) {
                    skill.scope = skill_scope::conditional;
                    skill.active = false;
                } else {
                    skill.scope = skill_scope::always;
                    skill.active = true;
                }
            } else {
                skill.scope = skill_scope::always;
                skill.active = true;
            }

            if (skill.name.empty()) {
                return std::unexpected(skill_diagnostic{.code = "missing_name", .message = "skill is missing required name", .source_path = source_path});
            }
            if (skill.description.empty()) {
                return std::unexpected(skill_diagnostic{.code = "missing_description", .message = "skill is missing required description", .source_path = source_path});
            }

            std::string body;
            for (std::size_t i = close_index + 1; i < lines.size(); ++i) {
                if (!body.empty()) {
                    body.push_back('\n');
                }
                body.append(lines[i]);
            }
            skill.body = std::move(body);

            const auto source_path_fs = std::filesystem::path(source_path);
            auto canonical_source_path = source_path_fs;
            if (!workspace_root.empty() && canonical_source_path.is_relative()) {
                canonical_source_path = utils::resolve_relative_to(canonical_source_path, workspace_root);
            }
            skill.id = make_skill_id(source, canonical_source_path.lexically_normal(), skill.name);

            return skill;
        }

        static bool conditions_valid(const skill_view &skill, std::vector<skill_diagnostic> &diagnostics) {
            for (const auto &platform : skill.activation.platforms_any) {
                const auto normalized = ascii_to_lower_copy(platform);
                if (normalized != "linux" && normalized != "macos" && normalized != "windows") {
                    diagnostics.push_back(skill_diagnostic{.code = "invalid_platform", .message = "unknown platform in platforms_any", .source_path = skill.source_path});
                    return false;
                }
            }

            for (const auto &pattern : skill.activation.paths_any) {
                const auto wildcard_count = std::ranges::count(pattern, '*');
                if (wildcard_count > 1 && pattern.find("**") == std::string::npos) {
                    diagnostics.push_back(skill_diagnostic{.code = "invalid_glob", .message = "invalid glob syntax in paths_any", .source_path = skill.source_path});
                    return false;
                }
            }

            return true;
        }

        void publish_deduplicated(catalog_snapshot &snapshot) {
            if (snapshot.loaded.empty()) {
                return;
            }

            std::unordered_map<std::string, std::size_t> chosen_index_by_name;
            for (std::size_t index = 0; index < snapshot.loaded.size(); ++index) {
                const auto &candidate = snapshot.loaded[index];
                auto it = chosen_index_by_name.find(candidate.name);
                if (it == chosen_index_by_name.end()) {
                    chosen_index_by_name.insert_or_assign(candidate.name, index);
                    continue;
                }

                snapshot.diagnostics.push_back(skill_diagnostic{
                    .code = "name_collision", .message = "replaced previous skill with same name by later precedence entry", .source_path = candidate.source_path});
                it->second = index;
            }

            std::vector<skill_view> deduped;
            deduped.reserve(chosen_index_by_name.size());
            for (const auto &[name, index] : chosen_index_by_name) {
                static_cast<void>(name);
                deduped.push_back(snapshot.loaded[index]);
            }

            std::ranges::sort(deduped, [](const skill_view &left, const skill_view &right) {
                return left.name < right.name;
            });
            snapshot.skills = std::move(deduped);
        }

        [[nodiscard]]
        static bool evaluate_env_all(const std::vector<std::string> &env_all) {
            for (const auto &env_name : env_all) {
                if (std::getenv(env_name.c_str()) == nullptr) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]]
        static bool evaluate_platforms_any(const std::vector<std::string> &platforms_any) {
            if (platforms_any.empty()) {
                return true;
            }

            std::string current_platform = "linux";
#ifdef _WIN32
            current_platform = "windows";
#elif defined(__APPLE__)
            current_platform = "macos";
#endif

            for (const auto &platform : platforms_any) {
                if (ascii_to_lower_copy(platform) == current_platform) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]]
        static bool evaluate_paths_any(const std::vector<std::string> &paths_any, const std::vector<std::filesystem::path> &paths, const std::filesystem::path &workspace_root) {
            if (paths_any.empty()) {
                return true;
            }
            for (const auto &path : paths) {
                auto relative = path;
                if (!workspace_root.empty() && path.is_absolute()) {
                    std::error_code ec;
                    relative = std::filesystem::relative(path, workspace_root, ec);
                    if (ec) {
                        relative = path;
                    }
                }

                const auto relative_text = relative.lexically_normal().generic_string();
                for (const auto &pattern : paths_any) {
                    if (permissions::matches_wildcard(pattern, relative_text)) {
                        return true;
                    }
                }
            }
            return false;
        }
    };

} // namespace orangutan::skills
