#include "memory/memory-mirror.hpp"
#include "utils/file-io.hpp"
#include "utils/file.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"
#include "utils/time-format.hpp"

#include <filesystem>

namespace orangutan::memory {
    namespace {

        constexpr std::string_view MANAGED_BEGIN_MARKER = "<!-- ORANGUTAN:MEMORY:BEGIN -->";
        constexpr std::string_view MANAGED_END_MARKER = "<!-- ORANGUTAN:MEMORY:END -->";

        std::string render_managed_block(const bootstrap::RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records) {
            std::string out;
            out.append(MANAGED_BEGIN_MARKER);
            out.push_back('\n');
            utils::format_to(out, "Generated durable memory for scope `{}`. Edit outside this managed block only.\n\n", context.scope);
            if (durable_records.empty()) {
                out.append("- No durable memories captured yet.\n");
            } else {
                for (const auto &record : durable_records) {
                    utils::format_to(out, "- [{}:{}:{}] {}", magic_enum::enum_name(record.type), record.category, record.key, record.content);
                    if (!record.source.empty()) {
                        utils::format_to(out, " {{source={}}}", record.source);
                    }
                    out.push_back('\n');
                }
            }
            out.append(MANAGED_END_MARKER);
            out.push_back('\n');
            return out;
        }

    } // namespace

    MemoryMirrorRefreshResult MemoryMirror::refresh_snapshot(const bootstrap::RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records) {
        MemoryMirrorRefreshResult result{.path = context.snapshot_path()};
        if (!context.mirror_enabled()) {
            result.skipped = true;
            result.status = "Mirror disabled or workspace unavailable.";
            return result;
        }

        const auto rendered = render_managed_block(context, durable_records);
        if (!std::filesystem::exists(result.path)) {
            std::filesystem::create_directories(result.path.parent_path());
            fileio::write_file(result.path, rendered);
            result.refreshed = true;
            result.status = "Created memory snapshot.";
            return result;
        }

        const auto current = fileio::read_file(result.path);
        const auto begin = current.find(MANAGED_BEGIN_MARKER);
        const auto end = current.find(MANAGED_END_MARKER);
        const auto second_begin = begin == std::string::npos ? std::string::npos : current.find(MANAGED_BEGIN_MARKER, begin + MANAGED_BEGIN_MARKER.size());
        const auto second_end = end == std::string::npos ? std::string::npos : current.find(MANAGED_END_MARKER, end + MANAGED_END_MARKER.size());
        if (begin == std::string::npos || end == std::string::npos || end < begin || second_begin != std::string::npos || second_end != std::string::npos) {
            result.skipped = true;
            result.status = "Existing snapshot markers are missing or malformed; refresh skipped.";
            return result;
        }

        const auto suffix_start = end + MANAGED_END_MARKER.size();
        const auto updated = current.substr(0, begin) + rendered + current.substr(suffix_start);
        fileio::write_file(result.path, updated);
        result.refreshed = true;
        result.status = "Refreshed memory snapshot.";
        return result;
    }

    JournalMirrorWriteResult MemoryMirror::append_daily_journal(const bootstrap::RuntimeMemoryContext &context, const std::string &summary) {
        JournalMirrorWriteResult result;
        if (!context.mirror_enabled()) {
            result.status = "Mirror disabled or workspace unavailable.";
            return result;
        }

        auto trimmed_summary = utils::trim_copy(summary);
        if (trimmed_summary.empty()) {
            result.status = "Journal summary is empty.";
            return result;
        }

        const auto journal_dir = context.journal_dir();
        std::filesystem::create_directories(journal_dir);
        result.path = journal_dir / (time::current_local_date() + ".md");

        std::error_code ec;
        bool has_existing_content = false;
        if (std::filesystem::exists(result.path, ec) && ec == std::error_code{}) {
            has_existing_content = std::filesystem::file_size(result.path, ec) > 0 && ec == std::error_code{};
        }

        fileio::File file(result.path, "a");
        if (has_existing_content) {
            fmt::println(file.get(), "");
            fmt::println(file.get(), "");
        }
        fmt::println(file.get(), "## {}", time::current_local_timestamp());
        fmt::print(file.get(), "{}", trimmed_summary);
        fmt::println(file.get(), "");
        file.close();

        result.mirrored = true;
        result.status = "Appended journal summary.";
        return result;
    }

} // namespace orangutan::memory
