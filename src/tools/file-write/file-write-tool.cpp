#include "tools/internal.hpp"
#include "utils/file-io.hpp"

#include <filesystem>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

namespace orangutan::tools {
    namespace {

        std::string write_file(const nlohmann::json &input, const std::filesystem::path &workspace_root) {
            const auto path = resolve_tool_path(std::filesystem::path(input.at("path").get<std::string>()), workspace_root);
            const auto content = input.at("content").get<std::string>();
            spdlog::info("  [tool] write: {}", path.string());

            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }

            fileio::write_file(path, content);

            return spdlog::fmt_lib::format("Wrote {} bytes to {}", content.size(), path.string());
        }

    } // namespace

    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root) {
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
             .execute = [workspace_root](const nlohmann::json &input) {
                 return write_file(input, workspace_root);
             }});
    }

} // namespace orangutan::tools
