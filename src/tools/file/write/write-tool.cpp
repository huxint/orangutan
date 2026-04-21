#include "tools/internal.hpp"
#include "tools/file/common.hpp"
#include "utils/file-io.hpp"
#include "utils/parallel.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace orangutan::tools {
    namespace {

        struct WriteJob {
            std::filesystem::path path;
            std::string content;
        };

        std::string write_one(const WriteJob &job) {
            spdlog::info("  [tool] write: {}", job.path.string());
            if (job.path.has_parent_path()) {
                std::filesystem::create_directories(job.path.parent_path());
            }
            fileio::write_file(job.path, job.content);
            return fmt::format("Wrote {} bytes to {}", job.content.size(), job.path.string());
        }

        std::vector<WriteJob> collect_jobs(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            const bool has_single = input.contains("path") && !input.at("path").is_null();
            const bool has_batch = input.contains("files") && input.at("files").is_array();
            if (has_single == has_batch) {
                throw std::runtime_error("Provide either `path` + `content` or a `files` array, not both");
            }

            std::vector<WriteJob> jobs;
            if (has_single) {
                jobs.push_back({.path = file::resolve_path_field(input, "path", workspace_root, permissions), .content = input.at("content").get<std::string>()});
                return jobs;
            }

            if (input.at("files").empty()) {
                throw std::runtime_error("`files` must contain at least one entry");
            }

            jobs.reserve(input.at("files").size());
            for (const auto &entry : input.at("files")) {
                if (!entry.is_object() || !entry.contains("path") || !entry.contains("content")) {
                    throw std::runtime_error("each `files` entry must be an object with `path` and `content`");
                }
                jobs.push_back({.path = file::resolve_path_field(entry, "path", workspace_root, permissions), .content = entry.at("content").get<std::string>()});
            }
            return jobs;
        }

        PermissionResult check_write_permissions(const ToolUse &call, const ToolPermissionContext &ctx, const std::filesystem::path &workspace_root) {
            if (call.input.contains("files") && call.input.at("files").is_array()) {
                if (call.input.at("files").empty()) {
                    return PermissionResult::deny("`files` must contain at least one entry");
                }
                for (const auto &entry : call.input.at("files")) {
                    if (!entry.is_object()) {
                        return PermissionResult::deny("each `files` entry must be an object");
                    }
                    if (auto r = file::validate_required_path(entry, "path", workspace_root, ctx); !r.is_passthrough) {
                        return r;
                    }
                }
                return PermissionResult::passthrough();
            }
            return file::validate_required_path(call.input, "path", workspace_root, ctx);
        }

        std::string write_batch(const nlohmann::json &input, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
            const auto jobs = collect_jobs(input, workspace_root, permissions);
            if (jobs.size() == 1) {
                return write_one(jobs.front());
            }

            // Writes fan out across the shared IO pool. parallel_map rethrows
            // the first exception after all tasks drain, so partial failures
            // surface but never leak inflight work.
            auto results = utils::parallel_map(std::span<const WriteJob>{jobs}, [](const WriteJob &job) { return write_one(job); });

            std::string out;
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (i > 0) {
                    out.push_back('\n');
                }
                out += results[i];
            }
            return out;
        }

    } // namespace

    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        registry.register_tool(
            {.definition = {.name = "write",
                            .description =
                                "Write content to one or more files, creating parent directories if needed.\n\n"
                                "Usage:\n"
                                " - Single file: pass `path` and `content`.\n"
                                " - Batch: pass `files: [{path, content}, …]` to write many files concurrently.\n"
                                " - This tool overwrites existing files at the given paths.\n"
                                " - If modifying an existing file, you MUST use the `read` tool first. Prefer the `edit` tool for modifications — it only sends the diff.\n"
                                " - Only use this tool to create new files or for complete rewrites.\n"
                                " - NEVER create documentation files (*.md, README) unless explicitly requested.\n"
                                " - Paths are confined to the workspace or ~/.orangutan configuration area.",
                            .input_schema = {{"type", "object"},
                                             {"properties",
                                              {{"path", {{"type", "string"}, {"description", "Single-file path (use with `content`)"}}},
                                               {"content", {{"type", "string"}, {"description", "Single-file contents"}}},
                                               {"files",
                                                {{"type", "array"},
                                                 {"description", "Batch form: array of `{path, content}` objects written in parallel"},
                                                 {"items",
                                                  {{"type", "object"},
                                                   {"properties",
                                                    {{"path", {{"type", "string"}}},
                                                     {"content", {{"type", "string"}}}}},
                                                   {"required", nlohmann::json::array({"path", "content"})}}}}}}}}},
             .check_permissions =
                 [workspace_root](const ToolUse &call, const ToolPermissionContext &ctx) {
                     return check_write_permissions(call, ctx, workspace_root);
                 },
             .execute =
                 [workspace_root, permissions](const nlohmann::json &input) {
                     return write_batch(input, workspace_root, permissions);
                 }});
    }

} // namespace orangutan::tools
