#pragma once

#include "features/memory/memory.hpp"
#include "app/runtime/memory-context.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orangutan {

struct MemoryMirrorRefreshResult {
    bool refreshed = false;
    bool skipped = false;
    std::string status;
    std::filesystem::path path;
};

struct JournalMirrorWriteResult {
    bool mirrored = false;
    std::string status;
    std::filesystem::path path;
};

class MemoryMirror {
public:
    [[nodiscard]]
    MemoryMirrorRefreshResult refresh_snapshot(const RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records) const;

    [[nodiscard]]
    JournalMirrorWriteResult append_daily_journal(const RuntimeMemoryContext &context, const std::string &summary) const;
};

} // namespace orangutan
