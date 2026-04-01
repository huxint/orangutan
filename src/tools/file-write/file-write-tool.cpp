#include "tools/internal.hpp"
#include "utils/file-io.hpp"

#include <filesystem>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

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
                            .description = "Write content to a file inside the current workspace or ~/.orangutan configuration area, creating parent directories if needed.",
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
