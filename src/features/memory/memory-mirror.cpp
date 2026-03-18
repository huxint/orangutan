#include "features/memory/memory-mirror.hpp"

#include "features/memory/memory-search.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace orangutan {
namespace {

constexpr std::string_view managed_begin_marker = "<!-- ORANGUTAN:MEMORY:BEGIN -->";
constexpr std::string_view managed_end_marker = "<!-- ORANGUTAN:MEMORY:END -->";

std::string current_date_string() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time, &local_tm);

    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y-%m-%d");
    return out.str();
}

std::string current_timestamp_string() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time, &local_tm);

    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string render_managed_block(const RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records) {
    std::ostringstream out;
    out << managed_begin_marker << '\n';
    out << "Generated durable memory for scope `" << context.scope << "`. Edit outside this managed block only.\n\n";
    if (durable_records.empty()) {
        out << "- No durable memories captured yet.\n";
    } else {
        for (const auto &record : durable_records) {
            out << "- [" << record.category << ':' << record.key << "] " << record.content;
            if (!record.source.empty()) {
                out << " {source=" << record.source << '}';
            }
            out << '\n';
        }
    }
    out << managed_end_marker << '\n';
    return out.str();
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path &path, const std::string &content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << content;
}

} // namespace

MemoryMirrorRefreshResult MemoryMirror::refresh_snapshot(const RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records) const {
    MemoryMirrorRefreshResult result{.path = context.snapshot_path()};
    if (!context.mirror_enabled()) {
        result.skipped = true;
        result.status = "Mirror disabled or workspace unavailable.";
        return result;
    }

    const auto rendered = render_managed_block(context, durable_records);
    if (!std::filesystem::exists(result.path)) {
        write_file(result.path, rendered);
        result.refreshed = true;
        result.status = "Created memory snapshot.";
        return result;
    }

    const auto current = read_file(result.path);
    const auto begin = current.find(managed_begin_marker);
    const auto end = current.find(managed_end_marker);
    const auto second_begin = begin == std::string::npos ? std::string::npos : current.find(managed_begin_marker, begin + managed_begin_marker.size());
    const auto second_end = end == std::string::npos ? std::string::npos : current.find(managed_end_marker, end + managed_end_marker.size());
    if (begin == std::string::npos || end == std::string::npos || end < begin || second_begin != std::string::npos || second_end != std::string::npos) {
        result.skipped = true;
        result.status = "Existing snapshot markers are missing or malformed; refresh skipped.";
        return result;
    }

    const auto suffix_start = end + managed_end_marker.size();
    const auto updated = current.substr(0, begin) + rendered + current.substr(suffix_start);
    write_file(result.path, updated);
    result.refreshed = true;
    result.status = "Refreshed memory snapshot.";
    return result;
}

JournalMirrorWriteResult MemoryMirror::append_daily_journal(const RuntimeMemoryContext &context, const std::string &summary) const {
    JournalMirrorWriteResult result;
    if (!context.mirror_enabled()) {
        result.status = "Mirror disabled or workspace unavailable.";
        return result;
    }

    const auto trimmed_summary = memory_detail::trim_copy(summary);
    if (trimmed_summary.empty()) {
        result.status = "Journal summary is empty.";
        return result;
    }

    const auto journal_dir = context.journal_dir();
    std::filesystem::create_directories(journal_dir);
    result.path = journal_dir / (current_date_string() + ".md");

    std::ofstream output(result.path, std::ios::app);
    if (output.tellp() > 0) {
        output << "\n\n";
    }
    output << "## " << current_timestamp_string() << "\n";
    output << trimmed_summary;
    output << '\n';

    result.mirrored = true;
    result.status = "Appended journal summary.";
    return result;
}

} // namespace orangutan
