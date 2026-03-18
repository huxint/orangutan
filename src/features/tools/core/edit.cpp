#include "features/tools/core/internal.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan {
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
    std::vector<std::pair<size_t, size_t>> match_positions;
    bool is_new_file = false;
};

std::vector<FilePatch> parse_patch(std::string_view patch) {
    if (patch.empty()) {
        throw std::runtime_error("patch is empty");
    }

    std::vector<FilePatch> files;
    std::istringstream stream{std::string(patch)};
    std::string line;

    constexpr std::string_view file_header = "*** ";
    constexpr std::string_view search_marker = "<<<<<<< SEARCH";
    constexpr std::string_view separator = "=======";
    constexpr std::string_view replace_marker = ">>>>>>> REPLACE";

    enum class State {
        idle,
        search,
        replace,
    };
    auto state = State::idle;
    std::string search_buf;
    std::string replace_buf;

    while (std::getline(stream, line)) {
        switch (state) {
            case State::idle:
                if (line.starts_with(file_header)) {
                    auto path = line.substr(file_header.size());
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
                } else if (line == search_marker) {
                    if (files.empty()) {
                        throw std::runtime_error("hunk before any *** file header");
                    }
                    state = State::search;
                    search_buf.clear();
                }
                break;

            case State::search:
                if (line == separator) {
                    state = State::replace;
                    replace_buf.clear();
                } else {
                    if (!search_buf.empty()) {
                        search_buf += '\n';
                    }
                    search_buf += line;
                }
                break;

            case State::replace:
                if (line == replace_marker) {
                    files.back().hunks.push_back({.search = std::move(search_buf), .replace = std::move(replace_buf)});
                    search_buf.clear();
                    replace_buf.clear();
                    state = State::idle;
                } else {
                    if (!replace_buf.empty()) {
                        replace_buf += '\n';
                    }
                    replace_buf += line;
                }
                break;
        }
    }

    if (state == State::search) {
        throw std::runtime_error("unclosed hunk: missing ======= separator");
    }
    if (state == State::replace) {
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

std::vector<ValidatedFile> validate_hunks(const std::vector<FilePatch> &files, const std::filesystem::path &workspace_root) {
    std::vector<ValidatedFile> validated;
    validated.reserve(files.size());

    for (const auto &file : files) {
        auto resolved = resolve_tool_path(std::filesystem::path(file.path), workspace_root);
        ValidatedFile validated_file{.resolved = std::move(resolved)};

        for (size_t i = 0; i < file.hunks.size(); ++i) {
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
                std::ifstream ifs(validated_file.resolved);
                if (!ifs) {
                    throw std::runtime_error("cannot open file: " + file.path);
                }
                std::ostringstream ss;
                ss << ifs.rdbuf();
                validated_file.content = ss.str();
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
    for (size_t file_index = 0; file_index < validated.size(); ++file_index) {
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

            std::ofstream ofs(validated_file.resolved);
            if (!ofs) {
                throw std::runtime_error("cannot create file: " + file.path);
            }
            ofs << content;
            continue;
        }

        auto positions = validated_file.match_positions;
        std::ranges::sort(positions, [](const auto &left, const auto &right) {
            return left.first > right.first;
        });

        for (const auto &[pos, hunk_index] : positions) {
            const auto &hunk = file.hunks[hunk_index];
            validated_file.content.replace(pos, hunk.search.size(), hunk.replace);
        }

        std::ofstream ofs(validated_file.resolved);
        if (!ofs) {
            throw std::runtime_error("cannot write file: " + file.path);
        }
        ofs << validated_file.content;
    }
}

std::string execute_edit_tool(const json &input, const std::filesystem::path &workspace_root) {
    const auto patch = input.at("patch").get<std::string>();
    spdlog::info("  [tool] edit: {} bytes", patch.size());

    const auto files = parse_patch(patch);
    auto validated = validate_hunks(files, workspace_root);
    apply_hunks(validated, files);

    size_t total_hunks = 0;
    std::string summary;
    for (const auto &file : files) {
        total_hunks += file.hunks.size();
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += file.path + " (" + std::to_string(file.hunks.size()) + (file.hunks.size() == 1 ? " hunk)" : " hunks)");
    }

    return "Applied " + std::to_string(total_hunks) + (total_hunks == 1 ? " hunk" : " hunks") + " across " + std::to_string(files.size()) +
           (files.size() == 1 ? " file: " : " files: ") + summary;
}

} // namespace

void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root) {
    registry.register_tool(
        {.definition = {.name = "edit",
                        .description = "Apply a multi-file, multi-hunk search/replace patch atomically. All hunks are validated before any file is written.",
                        .input_schema = {{"type", "object"},
                                         {"properties",
                                          {{"patch",
                                            {{"type", "string"},
                                             {"description", "Patch text with *** <path> file headers and <<<<<<< SEARCH / ======= / >>>>>>> REPLACE hunk markers"}}}}},
                                         {"required", json::array({"patch"})}}},
         .execute = [workspace_root](const json &input) {
             return execute_edit_tool(input, workspace_root);
         }});
}

} // namespace orangutan
