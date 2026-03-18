#include "features/tools/core/internal.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace orangutan {
namespace {

bool is_binary_file(const std::filesystem::path &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }

    constexpr size_t sample_size = 8192;
    std::vector<char> buf(sample_size);
    ifs.read(buf.data(), static_cast<std::streamsize>(sample_size));
    buf.resize(static_cast<size_t>(ifs.gcount()));
    return std::ranges::any_of(buf, [](char ch) {
        return ch == '\0';
    });
}

std::string read_single_file(const std::filesystem::path &path, int offset, int limit) {
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
        return "Binary file: " + path.string() + " (" + std::to_string(size) + " bytes, type: " + ext + ")";
    }

    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        lines.push_back(std::move(line));
    }

    const auto total_lines = static_cast<int>(lines.size());
    if (offset > total_lines) {
        return "No content at offset " + std::to_string(offset) + " (file has " + std::to_string(total_lines) + " lines)";
    }

    const int start = offset;
    const int end = std::min(offset + limit - 1, total_lines);
    const int output_count = end - start + 1;
    const int num_width = std::max(6, static_cast<int>(std::to_string(total_lines).size()));

    std::ostringstream out;
    for (int i = start; i <= end; ++i) {
        out << std::setw(num_width) << i << '\t' << lines[static_cast<size_t>(i - 1)] << '\n';
    }

    if (end < total_lines) {
        out << "... (showing " << output_count << " of " << total_lines << " lines, use offset to read more)\n";
    }

    return out.str();
}

std::string read_file(const json &input, const std::filesystem::path &workspace_root) {
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

    if (has_path) {
        const auto path = resolve_tool_path(std::filesystem::path(input.at("path").get<std::string>()), workspace_root);
        return read_single_file(path, offset, limit);
    }

    const auto &paths = input.at("paths");
    std::ostringstream out;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto path = resolve_tool_path(std::filesystem::path(paths[i].get<std::string>()), workspace_root);
        if (i > 0) {
            out << '\n';
        }
        out << "=== " << path.string() << " ===\n";

        try {
            out << read_single_file(path, offset, limit);
        } catch (const std::exception &e) {
            out << "Error: " << e.what() << '\n';
        }
    }

    return out.str();
}

} // namespace

void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root) {
    registry.register_tool(
        {.definition = {.name = "read",
                        .description = "Read file contents within the current workspace or ~/.orangutan configuration area with line numbers. Supports offset/limit for pagination, binary detection, and multi-path reading.",
                        .input_schema =
                            {{"type", "object"},
                             {"properties",
                              {{"path",
                                {{"type", "string"},
                                 {"description",
                                  "Workspace-relative file path, or an absolute/~ path inside the workspace or ~/.orangutan configuration area"}}},
                               {"paths",
                                {{"type", "array"},
                                 {"items", {{"type", "string"}}},
                                 {"description", "Array of file paths confined to the workspace or ~/.orangutan configuration area (use instead of 'path' for multi-file reads)"}}},
                               {"offset", {{"type", "integer"}, {"description", "1-based starting line number (default: 1)"}}},
                               {"limit", {{"type", "integer"}, {"description", "Maximum lines to return (default: 2000)"}}}}}}},
         .execute = [workspace_root](const json &input) {
             return read_file(input, workspace_root);
         }});
}

} // namespace orangutan
