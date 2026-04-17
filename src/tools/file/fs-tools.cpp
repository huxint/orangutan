#include "tools/file/file-common.hpp"
#include "tools/internal.hpp"
#include "utils/format.hpp"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace orangutan::tools {

    namespace fs = std::filesystem;

    namespace {

        std::string describe_error(const std::error_code &ec, std::string_view context, const fs::path &path) {
            return std::string{context} + " '" + path.string() + "': " + ec.message();
        }

        std::string format_entry(const fs::directory_entry &entry) {
            std::error_code ec;
            const auto is_dir = entry.is_directory(ec);
            const auto is_symlink = entry.is_symlink(ec);
            const char type = is_symlink ? 'l' : (is_dir ? 'd' : '-');

            std::string line;
            line.push_back(type);
            line.push_back(' ');
            line += entry.path().filename().string();
            if (!is_dir && !is_symlink) {
                const auto size = entry.file_size(ec);
                if (!ec) {
                    line += "  (";
                    line += std::to_string(size);
                    line += " bytes)";
                }
            } else if (is_dir) {
                line.push_back('/');
            }
            return line;
        }

        std::string do_ls(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto path = file::resolve_path_field(input, "path", workspace_root, permissions);
            spdlog::info("  [tool] ls: {}", path.string());

            std::error_code ec;
            if (!fs::is_directory(path, ec) || ec) {
                throw std::runtime_error("not a directory: " + path.string());
            }

            const bool recursive = input.value("recursive", false);
            const bool include_hidden = input.value("include_hidden", false);

            std::vector<std::string> lines;
            auto visit = [&](const fs::directory_entry &entry) {
                if (!include_hidden && entry.path().filename().string().starts_with('.')) {
                    return;
                }
                lines.push_back(format_entry(entry));
            };

            if (recursive) {
                for (const auto &entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
                    visit(entry);
                }
            } else {
                for (const auto &entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
                    visit(entry);
                }
            }
            if (ec) {
                throw std::runtime_error(describe_error(ec, "failed to list", path));
            }

            std::ranges::sort(lines);
            std::string out = path.string() + " (" + std::to_string(lines.size()) + " entries)\n";
            for (const auto &line : lines) {
                out += line;
                out.push_back('\n');
            }
            return out;
        }

        bool path_matches_glob(const fs::path &path, std::string_view pattern) {
            // Simple ** / * / ? glob: convert to a regex-free char-by-char matcher.
            const auto str = path.generic_string();
            std::size_t i = 0;
            std::size_t j = 0;
            std::size_t star_i = std::string::npos;
            std::size_t star_j = 0;
            while (i < str.size()) {
                if (j < pattern.size() && (pattern[j] == '?' || pattern[j] == str[i])) {
                    ++i;
                    ++j;
                } else if (j < pattern.size() && pattern[j] == '*') {
                    star_i = i;
                    star_j = j++;
                } else if (star_i != std::string::npos) {
                    j = star_j + 1;
                    i = ++star_i;
                } else {
                    return false;
                }
            }
            while (j < pattern.size() && pattern[j] == '*') {
                ++j;
            }
            return j == pattern.size();
        }

        std::string do_glob(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto base = input.contains("path") ? file::resolve_path_field(input, "path", workspace_root, permissions) : fs::path{workspace_root};
            const auto pattern = input.at("pattern").get<std::string>();
            const std::size_t max_results = input.value<std::size_t>("max_results", 500);
            spdlog::info("  [tool] glob: {} pattern={}", base.string(), pattern);

            std::error_code ec;
            std::vector<std::string> matches;
            for (const auto &entry : fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
                if (!entry.is_regular_file(ec) || ec) {
                    continue;
                }
                const auto rel = fs::relative(entry.path(), base, ec).generic_string();
                if (path_matches_glob(rel, pattern)) {
                    matches.emplace_back(std::move(rel));
                    if (matches.size() >= max_results) {
                        break;
                    }
                }
            }

            std::ranges::sort(matches);
            std::string out = "Matches under " + base.string() + " (" + std::to_string(matches.size()) + "):\n";
            for (const auto &m : matches) {
                out += m;
                out.push_back('\n');
            }
            return out;
        }

        std::string do_mkdir(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto path = file::resolve_path_field(input, "path", workspace_root, permissions);
            spdlog::info("  [tool] mkdir: {}", path.string());

            std::error_code ec;
            const bool created = fs::create_directories(path, ec);
            if (ec) {
                throw std::runtime_error(describe_error(ec, "failed to create directory", path));
            }
            return created ? ("Created directory " + path.string()) : ("Directory already exists: " + path.string());
        }

        std::string do_delete(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto path = file::resolve_path_field(input, "path", workspace_root, permissions);
            const bool recursive = input.value("recursive", false);
            spdlog::info("  [tool] delete: {} (recursive={})", path.string(), recursive);

            std::error_code ec;
            std::uintmax_t removed{};
            if (recursive) {
                removed = fs::remove_all(path, ec);
            } else {
                removed = fs::remove(path, ec) ? 1 : 0;
            }
            if (ec) {
                throw std::runtime_error(describe_error(ec, "failed to delete", path));
            }
            return "Removed " + std::to_string(removed) + " entr" + (removed == 1 ? "y: " : "ies under: ") + path.string();
        }

        std::string do_move(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto src = file::resolve_path_field(input, "source", workspace_root, permissions);
            const auto dst = file::resolve_path_field(input, "destination", workspace_root, permissions);
            spdlog::info("  [tool] move: {} -> {}", src.string(), dst.string());

            if (dst.has_parent_path()) {
                std::error_code mkdir_ec;
                fs::create_directories(dst.parent_path(), mkdir_ec);
            }

            std::error_code ec;
            fs::rename(src, dst, ec);
            if (ec) {
                // Fall back to copy + remove across filesystems.
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    throw std::runtime_error("failed to move '" + src.string() + "' to '" + dst.string() + "': " + ec.message());
                }
                fs::remove_all(src, ec);
                if (ec) {
                    throw std::runtime_error("moved but failed to remove source: " + ec.message());
                }
            }
            return "Moved " + src.string() + " to " + dst.string();
        }

        nlohmann::json path_prop(std::string_view description) {
            return {{"type", "string"}, {"description", std::string{description}}};
        }

        PermissionResult check_move_perms(const ToolUse &call, const ToolPermissionContext &ctx, const fs::path &workspace_root) {
            if (auto r = file::validate_required_path(call.input, "source", workspace_root, ctx); !r.is_passthrough) {
                return r;
            }
            return file::validate_required_path(call.input, "destination", workspace_root, ctx);
        }

    } // namespace

    void register_fs_tools(ToolRegistry &registry, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool({
            .definition = {.name = "ls",
                           .description = "List directory entries. Supports recursive traversal and hidden-file filtering.",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"path", path_prop("Directory path (workspace-relative or absolute within sandbox)")},
                                              {"recursive", {{"type", "boolean"}, {"description", "Recurse into subdirectories (default false)"}}},
                                              {"include_hidden", {{"type", "boolean"}, {"description", "Include dotfiles (default false)"}}}}},
                                            {"required", nlohmann::json::array({"path"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_required_path(c.input, "path", workspace_root, ctx);
            },
            .read_only = true,
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_ls(input, workspace_root, permissions); },
            .deferred = true,
        });

        registry.register_tool({
            .definition = {.name = "glob",
                           .description = "Find regular files under a directory whose relative path matches a glob pattern. "
                                          "Supports `*`, `?`, and `**` segments.",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"path", path_prop("Base directory (defaults to workspace root)")},
                                              {"pattern", {{"type", "string"}, {"description", "Glob pattern, e.g. `src/**/*.cpp`"}}},
                                              {"max_results", {{"type", "integer"}, {"description", "Cap on matches returned (default 500)"}}}}},
                                            {"required", nlohmann::json::array({"pattern"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_optional_path(c.input, "path", workspace_root, ctx);
            },
            .read_only = true,
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_glob(input, workspace_root, permissions); },
            .deferred = true,
        });

        registry.register_tool({
            .definition = {.name = "mkdir",
                           .description = "Create a directory, including any missing parents. Succeeds if already present.",
                           .input_schema = {{"type", "object"},
                                            {"properties", {{"path", path_prop("Directory path")}}},
                                            {"required", nlohmann::json::array({"path"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_required_path(c.input, "path", workspace_root, ctx);
            },
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_mkdir(input, workspace_root, permissions); },
            .deferred = true,
        });

        registry.register_tool({
            .definition = {.name = "delete",
                           .description = "Delete a file or (with recursive=true) a directory tree. Paths must stay inside the sandbox.",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"path", path_prop("Path to remove")},
                                              {"recursive", {{"type", "boolean"}, {"description", "Required for directories (default false)"}}}}},
                                            {"required", nlohmann::json::array({"path"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_required_path(c.input, "path", workspace_root, ctx);
            },
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_delete(input, workspace_root, permissions); },
            .deferred = true,
        });

        registry.register_tool({
            .definition = {.name = "move",
                           .description = "Move or rename a path. Falls back to recursive copy+delete across filesystem boundaries.",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"source", path_prop("Existing source path")},
                                              {"destination", path_prop("Target path")}}},
                                            {"required", nlohmann::json::array({"source", "destination"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return check_move_perms(c, ctx, workspace_root);
            },
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_move(input, workspace_root, permissions); },
            .deferred = true,
        });
    }

} // namespace orangutan::tools
