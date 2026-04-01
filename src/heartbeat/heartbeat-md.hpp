#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace orangutan::heartbeat {

    /// Load HEARTBEAT.md from disk. Returns std::nullopt if file doesn't exist.
    /// Returns empty string if the file is empty, whitespace-only, or contains only markdown headers (caller should skip the run).
    /// Returns file content if it has meaningful content.
    std::optional<std::string> load_heartbeat_md(const std::filesystem::path &path);

} // namespace orangutan::heartbeat

namespace orangutan {

    using heartbeat::load_heartbeat_md;

} // namespace orangutan
