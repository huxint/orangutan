#include "process/subprocess.hpp"
#include "tools/file/common.hpp"
#include "tools/internal.hpp"
#include "utils/format.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace orangutan::tools {

    namespace {

        namespace fs = std::filesystem;

        /// POSIX single-quote escape: wrap in '...', replacing any embedded ' with '\''.
        std::string shell_quote(std::string_view value) {
            std::string out;
            out.reserve(value.size() + 2);
            out.push_back('\'');
            for (const char ch : value) {
                if (ch == '\'') {
                    out += R"('\'')";
                } else {
                    out.push_back(ch);
                }
            }
            out.push_back('\'');
            return out;
        }

        std::string append_arg(std::string command, std::string_view flag, std::string_view value) {
            command.push_back(' ');
            command += flag;
            command.push_back(' ');
            command += shell_quote(value);
            return command;
        }

        struct SearchResult {
            std::string command;
            process::SubprocessResult result;
        };

        SearchResult run_search(const std::string &command, const fs::path &working_dir) {
            spdlog::info("  [tool] search: {}", command);
            process::SubprocessConfig config;
            config.command = command;
            config.working_dir = working_dir.string();
            config.use_shell = true;
            return SearchResult{.command = command, .result = process::run_subprocess(config)};
        }

        std::string summarise(const SearchResult &search, std::string_view empty_hint) {
            const auto &result = search.result;
            if (result.timed_out) {
                return "error: search timed out running `" + search.command + "`";
            }
            // rg exits 1 when there are no matches; fd exits 0. Treat 0 and 1 as success.
            if (result.exit_code != 0 && result.exit_code != 1) {
                return utils::format("error: `{}` exited with code {}\n{}", search.command, result.exit_code, result.stderr_output);
            }
            if (result.stdout_output.empty()) {
                return std::string{empty_hint};
            }
            return result.stdout_output;
        }

        fs::path resolve_search_root(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            if (input.contains("path") && input.at("path").is_string()) {
                return file::resolve_path_field(input, "path", workspace_root, permissions);
            }
            return fs::path{workspace_root};
        }

        std::string do_fd(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto root = resolve_search_root(input, workspace_root, permissions);
            const auto pattern = input.at("pattern").get<std::string>();
            const auto type = input.value("type", std::string{});
            const auto max_results = input.value<std::size_t>("max_results", 500);
            const bool include_hidden = input.value("include_hidden", false);

            std::string cmd = "fd --color=never";
            if (include_hidden) {
                cmd += " --hidden";
            }
            if (!type.empty()) {
                cmd = append_arg(std::move(cmd), "--type", type);
            }
            cmd = append_arg(std::move(cmd), "--max-results", std::to_string(max_results));
            cmd.push_back(' ');
            cmd += shell_quote(pattern);
            cmd.push_back(' ');
            cmd += shell_quote(root.string());

            return summarise(run_search(cmd, workspace_root), "No matches.");
        }

        std::string do_rg(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto root = resolve_search_root(input, workspace_root, permissions);
            const auto pattern = input.at("pattern").get<std::string>();
            const auto glob = input.value("glob", std::string{});
            const auto file_type = input.value("type", std::string{});
            const auto max_count = input.value<std::size_t>("max_count", 200);
            const bool case_insensitive = input.value("case_insensitive", false);
            const bool files_with_matches = input.value("files_with_matches", false);

            std::string cmd = "rg --color=never --line-number --no-heading";
            if (case_insensitive) {
                cmd += " -i";
            }
            if (files_with_matches) {
                cmd += " --files-with-matches";
            }
            if (!glob.empty()) {
                cmd = append_arg(std::move(cmd), "--glob", glob);
            }
            if (!file_type.empty()) {
                cmd = append_arg(std::move(cmd), "--type", file_type);
            }
            cmd = append_arg(std::move(cmd), "--max-count", std::to_string(max_count));
            cmd.push_back(' ');
            cmd += shell_quote(pattern);
            cmd.push_back(' ');
            cmd += shell_quote(root.string());

            return summarise(run_search(cmd, workspace_root), "No matches.");
        }

    } // namespace

    void register_file_search_tools(ToolRegistry &registry, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool({
            .definition = {.name = "fd",
                           .description = "Find files by name using the `fd` binary. Pattern is a regex over path components. "
                                          "`type` accepts fd values (f, d, l, x, e, s). Requires fd to be installed.",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"pattern", {{"type", "string"}, {"description", "Regex filename pattern"}}},
                                              {"path", {{"type", "string"}, {"description", "Search root (defaults to workspace)"}}},
                                              {"type", {{"type", "string"}, {"description", "fd --type value, e.g. 'f' or 'd'"}}},
                                              {"include_hidden", {{"type", "boolean"}, {"description", "Pass --hidden (default false)"}}},
                                              {"max_results", {{"type", "integer"}, {"description", "Cap (default 500)"}}}}},
                                            {"required", nlohmann::json::array({"pattern"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_optional_path(c.input, "path", workspace_root, ctx);
            },
            .read_only = true,
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_fd(input, workspace_root, permissions); },
            .deferred = true,
        });

        registry.register_tool({
            .definition = {.name = "rg",
                           .description = "Search file contents with the `rg` (ripgrep) binary. Returns matches in "
                                          "`path:line:text` form (or a file list when `files_with_matches` is set).",
                           .input_schema = {{"type", "object"},
                                            {"properties",
                                             {{"pattern", {{"type", "string"}, {"description", "Regex to search for"}}},
                                              {"path", {{"type", "string"}, {"description", "Search root (defaults to workspace)"}}},
                                              {"glob", {{"type", "string"}, {"description", "Path glob filter, e.g. '*.cpp'"}}},
                                              {"type", {{"type", "string"}, {"description", "rg --type value, e.g. 'cpp'"}}},
                                              {"case_insensitive", {{"type", "boolean"}, {"description", "Pass -i (default false)"}}},
                                              {"files_with_matches", {{"type", "boolean"}, {"description", "Only list files with matches"}}},
                                              {"max_count", {{"type", "integer"}, {"description", "Max matches per file (default 200)"}}}}},
                                            {"required", nlohmann::json::array({"pattern"})}}},
            .check_permissions = [workspace_root](const ToolUse &c, const ToolPermissionContext &ctx) {
                return file::validate_optional_path(c.input, "path", workspace_root, ctx);
            },
            .read_only = true,
            .execute = [workspace_root, permissions](const nlohmann::json &input) { return do_rg(input, workspace_root, permissions); },
            .deferred = true,
        });
    }

} // namespace orangutan::tools
