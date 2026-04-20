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

        std::string do_fd(const nlohmann::json &input, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
            if (!search::binary_available("fd")) {
                return search::missing_binary_error("fd", "Install it via your package manager (e.g. `apt install fd-find`, `brew install fd`).");
            }

            const auto root = input.contains("path") && input.at("path").is_string() ? file::resolve_path_field(input, "path", workspace_root, permissions) : fs::path{workspace_root};
            const auto pattern = input.at("pattern").get<std::string>();
            const auto type = input.value("type", std::string{});
            const auto max_results = input.value<std::size_t>("max_results", 500);
            const bool include_hidden = input.value("include_hidden", false);

            std::string cmd = "fd --color=never";
            if (include_hidden) {
                cmd += " --hidden";
            }
            if (!type.empty()) {
                cmd = search::append_arg(std::move(cmd), "--type", type);
            }
            cmd = search::append_arg(std::move(cmd), "--max-results", std::to_string(max_results));
            cmd.push_back(' ');
            cmd += utils::shell_single_quote_escape(pattern);
            cmd.push_back(' ');
            cmd += utils::shell_single_quote_escape(root.string());

            spdlog::info("  [tool] fd: {}", cmd);
            return search::summarise(search::run(cmd, workspace_root), "No matches.", /*no_match_exit_ok=*/false);
        }

    } // namespace

    void register_fd_tool(ToolRegistry &registry, const fs::path &workspace_root, const ToolPermissionContext *permissions) {
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
    }

} // namespace orangutan::tools
