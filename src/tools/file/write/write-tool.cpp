#include "tools/internal.hpp"
#include "tools/file/common.hpp"
#include "utils/file-io.hpp"
#include "utils/format.hpp"

#include <filesystem>
#include <spdlog/spdlog.h>

namespace orangutan::tools {
    namespace {

        std::string write_file(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto path = file::resolve_path_field(input, "path", workspace_root, permissions);
            const auto content = input.at("content").get<std::string>();
            spdlog::info("  [tool] write: {}", path.string());

            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }

            fileio::write_file(path, content);

            return utils::format("Wrote {} bytes to {}", content.size(), path.string());
        }

    } // namespace

    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool(
            {.definition = {.name = "write",
                            .description =
                                "Write content to a file, creating parent directories if needed.\n\n"
                                "Usage:\n"
                                " - This tool overwrites the existing file at the given path.\n"
                                " - If modifying an existing file, you MUST use the `read` tool first. Prefer the `edit` tool for modifications — it only sends the diff.\n"
                                " - Only use this tool to create new files or for complete rewrites.\n"
                                " - NEVER create documentation files (*.md, README) unless explicitly requested.\n"
                                " - Paths are confined to the workspace or ~/.orangutan configuration area.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"path",
                                                {{"type", "string"},
                                                 {"description", "Workspace-relative file path, or an absolute/~ path inside the workspace or ~/.orangutan configuration area"}}},
                                               {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
                                             {"required", nlohmann::json::array({"path", "content"})}}},
             .check_permissions =
                 [workspace_root](const ToolUse &call, const ToolPermissionContext &ctx) {
                     return file::validate_required_path(call.input, "path", workspace_root, ctx);
                 },
             .execute =
                 [workspace_root, permissions](const nlohmann::json &input) {
                     return write_file(input, workspace_root, permissions);
                 }});
    }

} // namespace orangutan::tools
