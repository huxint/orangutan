#include "tools/internal.hpp"
#include "tools/file-edit/hashline.hpp"
#include "infra/files/file-io.hpp"
#include "infra/files/file.hpp"

#include <algorithm>
#include <filesystem>
#include "infra/format.hpp"
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace orangutan {
    namespace {

        bool is_binary_file(const std::filesystem::path &path) {
            try {
                fileio::File file(path, "rb");

                constexpr std::size_t sample_size = 8192;
                std::vector<char> buf(sample_size);
                const auto bytes_read = std::fread(buf.data(), sizeof(char), buf.size(), file.get());
                if (std::ferror(file.get()) != 0) {
                    return false;
                }
                buf.resize(bytes_read);
                return std::ranges::any_of(buf, [](char ch) {
                    return ch == '\0';
                });
            } catch (const std::runtime_error &) {
                return false;
            }
        }

        std::string read_single_file(const std::filesystem::path &path, int offset, int limit, std::string_view edit_mode) {
            spdlog::info("  [tool] read: {}", path.string());

            if (!std::filesystem::exists(path)) {
                throw std::runtime_error("File not found: " + path.string());
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
                    append(out, "{:>{}}\t{}\n", i, num_width, lines[static_cast<std::size_t>(i - 1)]);
                }
            }

            if (end < total_lines) {
                append(out, "... (showing {} of {} lines, use offset to read more)\n", output_count, total_lines);
            }

            return out;
        }

        std::string read_file(const nlohmann::json &input, const std::filesystem::path &workspace_root, std::string_view edit_mode) {
            const bool has_path = input.contains("path") && !input["path"].is_null();
            const bool has_paths = input.contains("paths") && !input["paths"].is_null();

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
                const auto path = resolve_tool_path(std::filesystem::path(input.at("path").get<std::string>()), workspace_root);
                return read_single_file(path, offset, limit, edit_mode);
            }

            const auto &paths = input.at("paths");
            std::string out;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                const auto path = resolve_tool_path(std::filesystem::path(paths[i].get<std::string>()), workspace_root);
                if (i > 0) {
                    out.push_back('\n');
                }
                append(out, "=== {} ===\n", path.string());

                try {
                    out += read_single_file(path, offset, limit, edit_mode);
                } catch (const std::exception &e) {
                    append(out, "Error: {}\n", e.what());
                }
            }

            return out;
        }

    } // namespace

    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, std::string_view edit_mode) {
        registry.register_tool(
            {.definition =
                 {.name = "read",
                  .description =
                      "Read file contents within the current workspace or ~/.orangutan configuration area with line numbers. Supports offset/limit for pagination, binary "
                      "detection, and multi-path reading.",
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
             .execute = [workspace_root, mode = std::string(edit_mode)](const nlohmann::json &input) {
                 return read_file(input, workspace_root, mode);
             }});
    }

} // namespace orangutan
