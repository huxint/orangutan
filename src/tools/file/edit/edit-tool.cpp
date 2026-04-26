#include "tools/internal.hpp"
#include "tools/file/edit/hashline.hpp"
#include "utils/file-io.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"
#include "types/base.hpp"

#include <filesystem>
#include <spdlog/spdlog.h>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orangutan::tools {
    namespace {

        std::string render_lines(std::span<const std::string> lines, bool trailing_newline) {
            if (lines.empty()) {
                return {};
            }

            std::size_t total_size = trailing_newline ? 1 : 0;
            for (const auto &line : lines) {
                total_size += line.size();
            }
            total_size += lines.size() - 1;

            std::string output;
            output.reserve(total_size);
            output.append(lines.front());
            for (const auto &line : lines | std::views::drop(1)) {
                output.push_back('\n');
                output.append(line);
            }
            if (trailing_newline) {
                output.push_back('\n');
            }
            return output;
        }

        std::string execute_hashline_edit(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto path_str = input.at("path").get<std::string>();
            const auto &edits_json = input.at("edits");
            spdlog::info("  [tool] edit (hashline): {} edits on {}", edits_json.size(), path_str);

            auto resolved_path = resolve_tool_path(std::filesystem::path(path_str), workspace_root, permissions);

            if (!std::filesystem::exists(resolved_path)) {
                throw std::runtime_error("file not found: " + path_str);
            }

            // Read file into lines
            std::vector<std::string> lines;
            bool had_trailing_newline = false;
            {
                const auto content = fileio::read_file(resolved_path);
                had_trailing_newline = !content.empty() && content.back() == '\n';
                lines = utils::split_lines(content);
            }

            // Convert JSON edits to HashlineEdit structs
            std::vector<HashlineEdit> edits;
            edits.reserve(edits_json.size());
            for (const auto &edit_json : edits_json) {
                HashlineEdit edit;

                const auto op_str = edit_json.at("op").get<std::string>();
                if (op_str == "replace") {
                    edit.op = hashline_edit_op::replace;
                } else if (op_str == "insert_after") {
                    edit.op = hashline_edit_op::insert_after;
                } else if (op_str == "insert_before") {
                    edit.op = hashline_edit_op::insert_before;
                } else if (op_str == "delete") {
                    edit.op = hashline_edit_op::del;
                } else {
                    throw std::runtime_error("unknown edit op: " + op_str);
                }

                if (edit_json.contains("anchor")) {
                    edit.anchor = edit_json.at("anchor").get<std::string>();
                }
                if (edit_json.contains("end_anchor")) {
                    edit.end_anchor = edit_json.at("end_anchor").get<std::string>();
                }

                if (edit_json.contains("content")) {
                    const auto &content = edit_json.at("content");
                    if (content.is_string()) {
                        // Split string content on newlines
                        const auto content_str = content.get<std::string>();
                        std::istringstream stream(content_str);
                        std::string segment;
                        while (std::getline(stream, segment)) {
                            edit.content.push_back(std::move(segment));
                        }
                    } else if (content.is_array()) {
                        for (const auto &item : content) {
                            edit.content.push_back(item.get<std::string>());
                        }
                    }
                }

                edits.push_back(std::move(edit));
            }

            auto result = apply_hashline_edits(lines, edits);
            if (!result.ok) {
                throw std::runtime_error(result.error);
            }

            fileio::write_file(resolved_path, render_lines(result.lines, had_trailing_newline));

            std::string summary = fmt::format("Applied {}{} to {}", result.edits_applied, result.edits_applied == 1 ? " edit" : " edits", path_str);
            if (!result.warnings.empty()) {
                summary += "\nWarnings: " + result.warnings;
            }
            return summary;
        }

    } // namespace

    void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool(
            {.definition = {.name = "edit",
                            .description = "Edit a file using hash-anchored line references.\n\n"
                                           "Usage:\n"
                                           " - You MUST use the `read` tool first before editing. This tool will error if you haven't read the file.\n"
                                           " - ALWAYS prefer editing existing files. NEVER write new files unless explicitly required.\n"
                                           " - Lines are identified by LINE#HASH tags from the read tool output (e.g., \"42#KQ\").\n"
                                           " - If a hash doesn't match (file changed since read), the error shows the correct hashes.\n"
                                           "\n"
                                           "Operations:\n"
                                           "- replace: Replace line(s) at anchor (single) or anchor..end_anchor (range) with content\n"
                                           "- insert_after: Insert content after anchor line (omit anchor to append to EOF)\n"
                                           "- insert_before: Insert content before anchor line (omit anchor to prepend to BOF)\n"
                                           "- delete: Delete line at anchor (single) or anchor..end_anchor (range)",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"path", {{"type", "string"}, {"description", "File path to edit"}}},
                                               {"edits",
                                                {{"type", "array"},
                                                 {"items",
                                                  {{"type", "object"},
                                                   {"properties",
                                                    {{"op", {{"type", "string"}, {"enum", nlohmann::json::array({"replace", "insert_after", "insert_before", "delete"})}}},
                                                     {"anchor", {{"type", "string"}, {"description", "Line anchor in LINE#HASH format"}}},
                                                     {"end_anchor", {{"type", "string"}, {"description", "End anchor for range operations (inclusive)"}}},
                                                     {"content",
                                                      {{"oneOf", nlohmann::json::array({{{"type", "array"}, {"items", {{"type", "string"}}}}, {{"type", "string"}}})},
                                                       {"description", "Replacement/insertion lines. String content is split on newlines."}}}}},
                                                   {"required", nlohmann::json::array({"op"})}}}}}}},
                                             {"required", nlohmann::json::array({"path", "edits"})}}},
             .check_permissions =
                 [workspace_root](const ToolUse &call, const ToolPermissionContext &ctx) {
                     if (!call.input.contains("path") || !call.input["path"].is_string()) {
                         return PermissionResult::deny("Edit path is required");
                     }
                     return validate_path_permission(call.input.at("path").get<std::string>(), workspace_root, ctx);
                 },
             .execute =
                 [workspace_root, permissions](const nlohmann::json &input) {
                     return execute_hashline_edit(input, workspace_root, permissions);
                 }});
    }

} // namespace orangutan::tools
