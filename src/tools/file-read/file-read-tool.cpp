#include "tools/internal.hpp"
#include "tools/file-edit/hashline.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"
#include "utils/file.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <filesystem>
#include "utils/format.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace orangutan::tools {
    namespace {

        PermissionResult validate_read_permissions(const ToolUse &call, const ToolPermissionContext &ctx, const std::filesystem::path &workspace_root) {
            try {
                if (call.input.contains("path") && call.input["path"].is_string()) {
                    resolve_tool_path(std::filesystem::path(call.input.at("path").get<std::string>()), workspace_root, &ctx);
                }

                if (call.input.contains("paths") && call.input["paths"].is_array()) {
                    for (const auto &item : call.input.at("paths")) {
                        if (!item.is_string()) {
                            return PermissionResult::deny("Read path must be a string");
                        }
                        resolve_tool_path(std::filesystem::path(item.get<std::string>()), workspace_root, &ctx);
                    }
                }
            } catch (const std::exception &e) {
                return PermissionResult::deny(e.what());
            }

            return PermissionResult::passthrough();
        }

        constexpr std::size_t MAX_IMAGE_SIZE = std::size_t{5} * 1024 * 1024;

        constexpr std::string_view BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string encode_base64(std::string_view data) {
            std::string encoded;
            encoded.reserve(((data.size() + 2) / 3) * 4);
            for (std::size_t i = 0; i < data.size(); i += 3) {
                const unsigned int b0 = static_cast<unsigned char>(data[i]);
                const unsigned int b1 = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 0U;
                const unsigned int b2 = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 0U;
                const unsigned int combined = (b0 << 16U) | (b1 << 8U) | b2;
                encoded.push_back(BASE64_ALPHABET[(combined >> 18U) & 0x3FU]);
                encoded.push_back(BASE64_ALPHABET[(combined >> 12U) & 0x3FU]);
                encoded.push_back((i + 1 < data.size()) ? BASE64_ALPHABET[(combined >> 6U) & 0x3FU] : '=');
                encoded.push_back((i + 2 < data.size()) ? BASE64_ALPHABET[combined & 0x3FU] : '=');
            }
            return encoded;
        }

        bool is_image_extension(const std::filesystem::path &path) {
            const auto ext = path.extension().string();
            if (ext.size() < 2) {
                return false;
            }
            const auto lower = utils::ascii_to_lower_copy(ext);
            return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".gif" || lower == ".webp" || lower == ".bmp";
        }

        std::string guess_media_type(const std::filesystem::path &path) {
            const auto ext = path.extension().string();
            const auto lower = utils::ascii_to_lower_copy(ext);
            if (lower == ".png") {
                return "image/png";
            }
            if (lower == ".jpg" || lower == ".jpeg") {
                return "image/jpeg";
            }
            if (lower == ".gif") {
                return "image/gif";
            }
            if (lower == ".webp") {
                return "image/webp";
            }
            if (lower == ".bmp") {
                return "image/bmp";
            }
            return "application/octet-stream";
        }

        ToolOutput read_image_file(const std::filesystem::path &path) {
            const auto size = std::filesystem::file_size(path);
            if (size > MAX_IMAGE_SIZE) {
                return ToolOutput{spdlog::fmt_lib::format("Image too large to display: {} ({} bytes, limit {} bytes)", path.string(), size, MAX_IMAGE_SIZE)};
            }

            fileio::File file(path, "rb");
            std::string buf;
            buf.resize_and_overwrite(static_cast<std::size_t>(size), [&file](char *buffer, std::size_t buffer_size) {
                return std::fread(buffer, sizeof(char), buffer_size, file.get());
            });

            const auto media_type = guess_media_type(path);
            const auto b64 = encode_base64(buf);
            const auto description = spdlog::fmt_lib::format("Image: {} ({} bytes, {})", path.string(), size, media_type);
            return ToolOutput{description, {{.media_type = media_type, .data = b64}}};
        }

        bool is_binary_file(const std::filesystem::path &path) {
            try {
                fileio::File file(path, "rb");

                constexpr std::size_t SAMPLE_SIZE = 8192;
                std::string buf;
                buf.resize_and_overwrite(SAMPLE_SIZE, [&file](char *buffer, std::size_t buffer_size) {
                    return std::fread(buffer, sizeof(char), buffer_size, file.get());
                });
                if (std::ferror(file.get()) != 0) {
                    return false;
                }
                return std::ranges::any_of(buf, [](char ch) {
                    return ch == '\0';
                });
            } catch (const std::runtime_error &) {
                return false;
            }
        }

        ToolOutput read_single_file(const std::filesystem::path &path, int offset, int limit, std::string_view edit_mode) {
            spdlog::info("  [tool] read: {}", path.string());

            if (!std::filesystem::exists(path)) {
                throw std::runtime_error("File not found: " + path.string());
            }

            if (is_image_extension(path)) {
                return read_image_file(path);
            }

            if (is_binary_file(path)) {
                const auto size = std::filesystem::file_size(path);
                auto ext = path.extension().string();
                if (ext.empty()) {
                    ext = "unknown";
                }
                return spdlog::fmt_lib::format("Binary file: {} ({} bytes, type: {})", path.string(), size, ext);
            }

            const auto content = fileio::read_file(path);

            std::vector<std::string> lines;
            std::istringstream input(content);
            std::string line;
            while (std::getline(input, line)) {
                lines.push_back(std::move(line));
            }

            const auto total_lines = static_cast<int>(lines.size());
            if (offset > total_lines) {
                return spdlog::fmt_lib::format("No content at offset {} (file has {} lines)", offset, total_lines);
            }

            const int start = offset;
            const int end = std::min(offset + limit - 1, total_lines);
            const int output_count = end - start + 1;
            const int num_width = std::max(6, static_cast<int>(std::to_string(total_lines).size()));

            std::string out;
            for (int i = start; i <= end; ++i) {
                if (edit_mode == "hashline") {
                    out += format_hashline(lines[static_cast<std::size_t>(i - 1)], static_cast<std::size_t>(i));
                    out.push_back('\n');
                } else {
                    utils::format_to(out, "{:>{}}\t{}\n", i, num_width, lines[static_cast<std::size_t>(i - 1)]);
                }
            }

            if (end < total_lines) {
                utils::format_to(out, "... (showing {} of {} lines, use offset to read more)\n", output_count, total_lines);
            }

            return out;
        }

        ToolOutput read_file(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions, std::string_view edit_mode) {
            const bool has_path = input.contains("path") && !input["path"].is_null();
            const bool has_paths = input.contains("paths") && !input["paths"].is_null() && !(input["paths"].is_array() && input["paths"].empty());

            if (has_path && has_paths) {
                throw std::runtime_error("Provide either 'path' or 'paths', not both");
            }
            if (!has_path && !has_paths) {
                throw std::runtime_error("Required: 'path' (string) or 'paths' (array of strings)");
            }

            const int offset = input.value("offset", 1);
            const int limit = input.value("limit", 2000);
            if (offset < 1) {
                throw std::runtime_error("'offset' must be >= 1");
            }
            if (limit < 1) {
                throw std::runtime_error("'limit' must be >= 1");
            }

            if (has_path) {
                const auto path = resolve_tool_path(std::filesystem::path(input.at("path").get<std::string>()), workspace_root, permissions);
                return read_single_file(path, offset, limit, edit_mode);
            }

            const auto &paths = input.at("paths");
            ToolOutput combined;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                const auto path = resolve_tool_path(std::filesystem::path(paths[i].get<std::string>()), workspace_root, permissions);
                if (i > 0) {
                    combined.text.push_back('\n');
                }
                utils::format_to(combined.text, "=== {} ===\n", path.string());

                try {
                    auto single = read_single_file(path, offset, limit, edit_mode);
                    combined.text += single.text;
                    combined.images.insert(combined.images.end(), std::make_move_iterator(single.images.begin()), std::make_move_iterator(single.images.end()));
                } catch (const std::exception &e) {
                    utils::format_to(combined.text, "Error: {}\n", e.what());
                }
            }

            return combined;
        }

    } // namespace

    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions, std::string_view edit_mode) {
        registry.register_tool(
            {.definition =
                 {.name = "read",
                  .description = "Read file contents from the workspace with line numbers.\n\n"
                                 "Usage:\n"
                                 " - Paths are workspace-relative or absolute within the workspace / ~/.orangutan area.\n"
                                 " - By default reads up to 2000 lines from the start of the file.\n"
                                 " - Use offset and limit for pagination on large files.\n"
                                 " - Results use cat -n format with line numbers starting at 1.\n"
                                 " - Can read multiple files in one call using the `paths` array.\n"
                                 " - Binary files are detected automatically.\n"
                                 " - Image files (png, jpg, gif, webp, bmp) are read and displayed visually.\n"
                                 " - This tool can only read files, not directories. Use `shell` with `ls` for directories.",
                  .input_schema =
                      {{"type", "object"},
                       {"properties",
                        {{"path",
                          {{"type", "string"}, {"description", "Workspace-relative file path, or an absolute/~ path inside the workspace or ~/.orangutan configuration area"}}},
                         {"paths",
                          {{"type", "array"},
                           {"items", {{"type", "string"}}},
                           {"description", "Array of file paths confined to the workspace or ~/.orangutan configuration area (use instead of 'path' for multi-file reads)"}}},
                         {"offset", {{"type", "integer"}, {"description", "1-based starting line number (default: 1)"}}},
                         {"limit", {{"type", "integer"}, {"description", "Maximum lines to return (default: 2000)"}}}}}}},
             .check_permissions =
                 [workspace_root](const ToolUse &call, const ToolPermissionContext &ctx) {
                     return validate_read_permissions(call, ctx, workspace_root);
                 },
             .read_only = true,
             .execute_rich =
                 [workspace_root, permissions, mode = std::string(edit_mode)](const nlohmann::json &input) {
                     return read_file(input, workspace_root, permissions, mode);
                 }});
    }

} // namespace orangutan::tools
