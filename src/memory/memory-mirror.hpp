#pragma once

#include "memory/memory-store.hpp"
#include "bootstrap/memory-context.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orangutan::memory {

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
        static MemoryMirrorRefreshResult refresh_snapshot(const bootstrap::RuntimeMemoryContext &context, const std::vector<MemoryRecord> &durable_records);

        [[nodiscard]]
        static JournalMirrorWriteResult append_daily_journal(const bootstrap::RuntimeMemoryContext &context, const std::string &summary);
    };

} // namespace orangutan::memory

namespace orangutan {

    using memory::JournalMirrorWriteResult;
    using memory::MemoryMirror;
    using memory::MemoryMirrorRefreshResult;

} // namespace orangutan
