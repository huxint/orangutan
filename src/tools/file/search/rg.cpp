#include "tools/file/common.hpp"
#include "tools/file/search/common.hpp"
#include "tools/internal.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::tools {

    namespace {

        namespace fs = std::filesystem;
        namespace search = file::search;

        std::string do_rg(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            if (!search::binary_available("rg")) {
                return search::missing_binary_error("rg", "Install ripgrep (e.g. `apt install ripgrep`, `brew install ripgrep`).");
            }

            const auto root = input.contains("path") && input.at("path").is_string() ? file::resolve_path_field(input, "path", workspace_root, permissions) : fs::path{workspace_root};
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
                cmd = search::append_arg(std::move(cmd), "--glob", glob);
            }
            if (!file_type.empty()) {
                cmd = search::append_arg(std::move(cmd), "--type", file_type);
            }
            cmd = search::append_arg(std::move(cmd), "--max-count", std::to_string(max_count));
            cmd.push_back(' ');
            cmd += search::shell_quote(pattern);
            cmd.push_back(' ');
            cmd += search::shell_quote(root.string());

            spdlog::info("  [tool] rg: {}", cmd);
            // rg exits 1 when there are no matches — treat as success.
            return search::summarise(search::run(cmd, workspace_root), "No matches.", /*no_match_exit_ok=*/true);
        }

    } // namespace

    void register_rg_tool(ToolRegistry &registry, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
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
