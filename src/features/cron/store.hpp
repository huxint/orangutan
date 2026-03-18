#pragma once

#include <string>
#include <vector>

namespace orangutan {

struct CronJobEntry {
    std::string name;
    std::string cron;
    std::string prompt;
    std::string agent = "default";
    std::string channel = "cli";
};

class CronStore {
public:
    explicit CronStore(std::string path);

    /// Load all jobs from disk. Returns empty vector on missing or corrupt file.
    [[nodiscard]]
    std::vector<CronJobEntry> load() const;

    /// Save all jobs to disk atomically (write to temp file, rename).
    void save(const std::vector<CronJobEntry> &jobs) const;

    /// Add a job and persist. Returns false if name already exists.
    bool add(const CronJobEntry &entry);

    /// Remove a job by name and persist. Returns false if not found.
    bool remove(const std::string &name);

    /// Get all jobs (in-memory).
    [[nodiscard]]
    const std::vector<CronJobEntry> &jobs() const;

private:
    std::string path_;
    std::vector<CronJobEntry> jobs_;

    void ensure_directory() const;
};

} // namespace orangutan
