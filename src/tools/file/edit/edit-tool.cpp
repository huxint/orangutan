#include "tools/internal.hpp"
#include "tools/file/edit/hashline.hpp"
#include "utils/file-io.hpp"
#include "utils/format.hpp"
#include "utils/parallel.hpp"
#include "utils/string.hpp"
#include "types/base.hpp"

#include <algorithm>
#include <filesystem>
#include <numeric>
#include <ranges>
#include <spdlog/spdlog.h>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace orangutan::tools {
    namespace {

        struct PatchHunk {
            std::string search;
            std::string replace;
        };

        struct FilePatch {
            std::string path;
            std::vector<PatchHunk> hunks;
        };

        struct ValidatedFile {
            std::filesystem::path resolved;
            std::string content;
            std::vector<std::pair<std::size_t, std::size_t>> match_positions;
            bool is_new_file = false;
        };

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

        std::vector<FilePatch> parse_patch(std::string_view patch) {
            if (patch.empty()) {
                throw std::runtime_error("patch is empty");
            }

            std::vector<FilePatch> files;

            constexpr std::string_view FILE_HEADER = "*** ";
            constexpr std::string_view SEARCH_MARKER = "<<<<<<< SEARCH";
            constexpr std::string_view SEPARATOR = "=======";
            constexpr std::string_view REPLACE_MARKER = ">>>>>>> REPLACE";

            enum class state : std::uint8_t {
                idle,
                search,
                replace,
            };
            auto parse_state = state::idle;
            std::string search_buf;
            std::string replace_buf;

            for (const auto &line : utils::split_lines(patch)) {
                switch (parse_state) {
                    case state::idle:
                        if (line.starts_with(FILE_HEADER)) {
                            auto path = line.substr(FILE_HEADER.size());
                            while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\r')) {
                                path.pop_back();
                            }
                            if (path.empty()) {
                                throw std::runtime_error("file header has no path");
                            }

                            const auto it = std::ranges::find_if(files, [&](const FilePatch &file) {
                                return file.path == path;
                            });
                            if (it == files.end()) {
                                files.push_back({.path = std::move(path), .hunks = {}});
                            }
                        } else if (line == SEARCH_MARKER) {
                            if (files.empty()) {
                                throw std::runtime_error("hunk before any *** file header");
                            }
                            parse_state = state::search;
                            search_buf.clear();
                        }
                        break;

                    case state::search:
                        if (line == SEPARATOR) {
                            parse_state = state::replace;
                            replace_buf.clear();
                        } else {
                            if (!search_buf.empty()) {
                                search_buf += '\n';
                            }
                            search_buf += line;
                        }
                        break;

                    case state::replace:
                        if (line == REPLACE_MARKER) {
                            files.back().hunks.push_back({.search = std::move(search_buf), .replace = std::move(replace_buf)});
                            search_buf.clear();
                            replace_buf.clear();
                            parse_state = state::idle;
                        } else {
                            if (!replace_buf.empty()) {
                                replace_buf += '\n';
                            }
                            replace_buf += line;
                        }
                        break;
                }
            }

            if (parse_state == state::search) {
                throw std::runtime_error("unclosed hunk: missing ======= separator");
            }
            if (parse_state == state::replace) {
                throw std::runtime_error("unclosed hunk: missing >>>>>>> REPLACE marker");
            }

            const bool has_hunks = std::ranges::any_of(files, [](const FilePatch &file) {
                return !file.hunks.empty();
            });
            if (!has_hunks) {
                throw std::runtime_error("patch contains no hunks");
            }

            return files;
        }

        std::vector<ValidatedFile> validate_hunks(const std::vector<FilePatch> &files, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            std::vector<ValidatedFile> validated;
            validated.reserve(files.size());

            for (const auto &file : files) {
                auto resolved = resolve_tool_path(std::filesystem::path(file.path), workspace_root, permissions);
                ValidatedFile validated_file{.resolved = std::move(resolved)};

                for (std::size_t i = 0; i < file.hunks.size(); ++i) {
                    const auto &hunk = file.hunks[i];

                    if (hunk.search.empty()) {
                        if (std::filesystem::exists(validated_file.resolved)) {
                            throw std::runtime_error("file already exists: " + file.path);
                        }
                        validated_file.is_new_file = true;
                        validated_file.match_positions.emplace_back(0, i);
                        continue;
                    }

                    if (validated_file.content.empty() && !validated_file.is_new_file) {
                        if (!std::filesystem::exists(validated_file.resolved)) {
                            throw std::runtime_error("file not found: " + file.path);
                        }
                        validated_file.content = fileio::read_file(validated_file.resolved);
                    }

                    const auto pos = validated_file.content.find(hunk.search);
                    if (pos == std::string::npos) {
                        throw std::runtime_error("search text not found in " + file.path);
                    }

                    const auto second = validated_file.content.find(hunk.search, pos + 1);
                    if (second != std::string::npos) {
                        throw std::runtime_error("search text matches multiple locations in " + file.path);
                    }

                    validated_file.match_positions.emplace_back(pos, i);
                }

                validated.push_back(std::move(validated_file));
            }

            return validated;
        }

        void apply_hunks(std::vector<ValidatedFile> &validated, const std::vector<FilePatch> &files) {
            // Each file lives on disk independently. Parallelise the apply step
            // so patches touching many files write concurrently on the shared IO pool.
            std::vector<std::size_t> indices(validated.size());
            std::iota(indices.begin(), indices.end(), std::size_t{0});

            utils::parallel_map(std::span<const std::size_t>{indices}, [&](std::size_t file_index) -> std::monostate {
                auto &validated_file = validated[file_index];
                const auto &file = files[file_index];

                if (validated_file.is_new_file) {
                    if (validated_file.resolved.has_parent_path()) {
                        std::filesystem::create_directories(validated_file.resolved.parent_path());
                    }

                    std::string content;
                    for (const auto &hunk : file.hunks) {
                        content += hunk.replace;
                    }

                    fileio::write_file(validated_file.resolved, content);
                    return {};
                }

                auto positions = validated_file.match_positions;
                std::ranges::sort(positions, [](const auto &left, const auto &right) {
                    return left.first > right.first;
                });

                for (const auto &[pos, hunk_index] : positions) {
                    const auto &hunk = file.hunks[hunk_index];
                    validated_file.content.replace(pos, hunk.search.size(), hunk.replace);
                }

                fileio::write_file(validated_file.resolved, validated_file.content);
                return {};
            });
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

        std::string execute_edit_tool(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto patch = input.at("patch").get<std::string>();
            spdlog::info("  [tool] edit: {} bytes", patch.size());

            const auto files = parse_patch(patch);
            auto validated = validate_hunks(files, workspace_root, permissions);
            apply_hunks(validated, files);

            std::size_t total_hunks = 0;
            std::string summary;
            for (const auto &file : files) {
                total_hunks += file.hunks.size();
                if (!summary.empty()) {
                    summary += ", ";
                }
                utils::format_to(summary, "{} ({} {})", file.path, file.hunks.size(), file.hunks.size() == 1 ? "hunk" : "hunks");
            }

            return fmt::format("Applied {} {} across {} {}: {}", total_hunks, total_hunks == 1 ? "hunk" : "hunks", files.size(), files.size() == 1 ? "file" : "files",
                                           summary);
        }

    } // namespace

    void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions, file::edit_mode mode) {
        if (mode == file::edit_mode::hashline) {
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
        } else {
            registry.register_tool(
                {.definition = {.name = "edit",
                                .description = "Apply a multi-file, multi-hunk search/replace patch atomically.\n\n"
                                               "Usage:\n"
                                               " - You MUST use the `read` tool first before editing. This tool will error if you haven't read the file.\n"
                                               " - ALWAYS prefer editing existing files. NEVER write new files unless explicitly required.\n"
                                               " - All hunks are validated before any file is written.\n"
                                               " - Paths must stay inside the workspace or ~/.orangutan configuration area.",
                                .input_schema = {{"type", "object"},
                                                 {"properties",
                                                  {{"patch",
                                                    {{"type", "string"},
                                                     {"description", "Patch text with *** <path> file headers and <<<<<<< SEARCH / ======= / >>>>>>> REPLACE hunk "
                                                                     "markers; paths must stay inside the workspace or ~/.orangutan configuration area"}}}}},
                                                 {"required", nlohmann::json::array({"patch"})}}},
                 .check_permissions =
                     [workspace_root](const ToolUse &call, const ToolPermissionContext &ctx) {
                         if (!call.input.contains("patch") || !call.input["patch"].is_string()) {
                             return PermissionResult::deny("Edit patch is required");
                         }

                         try {
                             const auto files = parse_patch(call.input.at("patch").get<std::string>());
                             for (const auto &file : files) {
                                 auto result = validate_path_permission(file.path, workspace_root, ctx);
                                 if (!result.is_passthrough) {
                                     return result;
                                 }
                             }
                         } catch (const std::exception &e) {
                             return PermissionResult::deny(e.what());
                         }

                         return PermissionResult::passthrough();
                     },
                 .execute =
                     [workspace_root, permissions](const nlohmann::json &input) {
                         return execute_edit_tool(input, workspace_root, permissions);
                     }});
        }
    }

} // namespace orangutan::tools
