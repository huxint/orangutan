#include "tools/internal.hpp"
#include "tools/file/edit/hashline.hpp"
#include "tools/file/common.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"
#include "utils/parallel.hpp"
#include "utils/file.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::tools {
    namespace {

        PermissionResult validate_read_permissions(const ToolUse &call, const ToolPermissionContext &ctx, const std::filesystem::path &workspace_root) {
            if (auto result = file::validate_optional_path(call.input, "path", workspace_root, ctx); !result.is_passthrough) {
                return result;
            }
            return file::validate_optional_paths(call.input, "paths", workspace_root, ctx);
        }

        constexpr std::size_t MAX_IMAGE_SIZE = std::size_t{5} * 1024 * 1024;

        constexpr std::string_view BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        constexpr std::array<std::pair<std::string_view, std::string_view>, 6> IMAGE_MEDIA_TYPES{{
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif", "image/gif"},
            {".webp", "image/webp"},
            {".bmp", "image/bmp"},
        }};

        [[nodiscard]]
        std::optional<std::string_view> lookup_image_media_type(const std::filesystem::path &path) {
            const auto ext = utils::ascii_to_lower_copy(path.extension().string());
            const auto it = std::ranges::find(IMAGE_MEDIA_TYPES, ext, &std::pair<std::string_view, std::string_view>::first);
            if (it == IMAGE_MEDIA_TYPES.end()) {
                return std::nullopt;
            }
            return it->second;
        }

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

        ToolOutput read_image_file(const std::filesystem::path &path, std::string_view media_type) {
            const auto size = std::filesystem::file_size(path);
            if (size > MAX_IMAGE_SIZE) {
                return ToolOutput{fmt::format("Image too large to display: {} ({} bytes, limit {} bytes)", path.string(), size, MAX_IMAGE_SIZE)};
            }

            fileio::File file(path, "rb");
            std::string buf;
            buf.resize_and_overwrite(static_cast<std::size_t>(size), [&file](char *buffer, std::size_t buffer_size) {
                return std::fread(buffer, sizeof(char), buffer_size, file.get());
            });

            auto media = std::string(media_type);
            const auto b64 = encode_base64(buf);
            const auto description = fmt::format("Image: {} ({} bytes, {})", path.string(), size, media);
            return ToolOutput{description, {{.media_type = std::move(media), .data = b64}}};
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

        ToolOutput read_single_file(const std::filesystem::path &path, int offset, int limit) {
            spdlog::info("  [tool] read: {}", path.string());

            if (!std::filesystem::exists(path)) {
                throw std::runtime_error("File not found: " + path.string());
            }

            if (const auto media_type = lookup_image_media_type(path); media_type.has_value()) {
                return read_image_file(path, *media_type);
            }

            if (is_binary_file(path)) {
                const auto size = std::filesystem::file_size(path);
                auto ext = path.extension().string();
                if (ext.empty()) {
                    ext = "unknown";
                }
                return fmt::format("Binary file: {} ({} bytes, type: {})", path.string(), size, ext);
            }

            const auto content = fileio::read_file(path);

            const auto lines = utils::split_lines(content);

            const auto total_lines = static_cast<int>(lines.size());
            if (offset > total_lines) {
                return fmt::format("No content at offset {} (file has {} lines)", offset, total_lines);
            }

            const int start = offset;
            const int end = std::min(offset + limit - 1, total_lines);
            const int output_count = end - start + 1;
            std::string out;
            for (int i = start; i <= end; ++i) {
                out += format_hashline(lines[static_cast<std::size_t>(i - 1)], static_cast<std::size_t>(i));
                out.push_back('\n');
            }

            if (end < total_lines) {
                utils::format_to(out, "... (showing {} of {} lines, use offset to read more)\n", output_count, total_lines);
            }

            return out;
        }

        ToolOutput read_file(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
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
                return read_single_file(path, offset, limit);
            }

            const auto &paths_json = input.at("paths");
            std::vector<std::filesystem::path> paths;
            paths.reserve(paths_json.size());
            for (const auto &entry : paths_json) {
                paths.push_back(resolve_tool_path(std::filesystem::path(entry.get<std::string>()), workspace_root, permissions));
            }

            // Fan out reads across the shared IO pool. Each read is independent
            // and exceptions are captured per-file so partial failures do not
            // mask successful reads.
            struct PerFile {
                ToolOutput output;
                std::string error;
            };
            auto results = utils::parallel_map(std::span<const std::filesystem::path>{paths}, [&](const std::filesystem::path &path) -> PerFile {
                try {
                    return {.output = read_single_file(path, offset, limit), .error = {}};
                } catch (const std::exception &e) {
                    return {.output = {}, .error = e.what()};
                }
            });

            ToolOutput combined;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                if (i > 0) {
                    combined.text.push_back('\n');
                }
                utils::format_to(combined.text, "=== {} ===\n", paths[i].string());
                if (!results[i].error.empty()) {
                    utils::format_to(combined.text, "Error: {}\n", results[i].error);
                    continue;
                }
                combined.text += results[i].output.text;
                combined.images.insert(combined.images.end(), std::make_move_iterator(results[i].output.images.begin()),
                                       std::make_move_iterator(results[i].output.images.end()));
            }

            return combined;
        }

    } // namespace

    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool(
            {.definition =
                 {.name = "read",
                  .description = "Read file contents from the workspace with line numbers.\n\n"
                                 "Usage:\n"
                                 " - Paths are workspace-relative or absolute within the workspace / ~/.orangutan area.\n"
                                 " - By default reads up to 2000 lines from the start of the file.\n"
                                  " - Use offset and limit for pagination on large files.\n"
                                  " - Results use LINE#HASH anchors so edits can detect stale reads.\n"
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
                 [workspace_root, permissions](const nlohmann::json &input) {
                     return read_file(input, workspace_root, permissions);
                 }});
    }

} // namespace orangutan::tools
