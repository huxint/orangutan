#include "features/cron/store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace orangutan {

CronStore::CronStore(std::string path)
: path_(std::move(path)) {
    jobs_ = load();
}

std::vector<CronJobEntry> CronStore::load() const {
    if (path_.empty()) {
        return {};
    }

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        return {};
    }

    std::ifstream file(path_);
    if (!file.is_open()) {
        spdlog::warn("Failed to open cron store at '{}'", path_);
        return {};
    }

    try {
        auto json = nlohmann::json::parse(file);
        if (!json.is_array()) {
            spdlog::warn("Cron store at '{}' is not a JSON array, treating as empty", path_);
            return {};
        }

        std::vector<CronJobEntry> result;
        for (const auto &item : json) {
            CronJobEntry entry;
            entry.name = item.value("name", "");
            entry.cron = item.value("cron", "");
            entry.prompt = item.value("prompt", "");
            entry.agent = item.value("agent", "default");
            entry.channel = item.value("channel", "cli");

            if (entry.name.empty() || entry.cron.empty() || entry.prompt.empty()) {
                spdlog::warn("Skipping cron job entry with missing required fields");
                continue;
            }

            result.push_back(std::move(entry));
        }

        return result;
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("Corrupt cron store at '{}': {}", path_, e.what());
        return {};
    }
}

bool CronStore::save(const std::vector<CronJobEntry> &jobs) const {
    if (path_.empty()) {
        return true;
    }

    if (!ensure_directory()) {
        return false;
    }

    auto json = nlohmann::json::array();
    for (const auto &job : jobs) {
        json.push_back({
            {"name", job.name},
            {"cron", job.cron},
            {"prompt", job.prompt},
            {"agent", job.agent},
            {"channel", job.channel},
        });
    }

    // Atomic write: write to temp file, then rename
    const auto temp_path = path_ + ".tmp";
    {
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            spdlog::error("Failed to open temp file for cron store: '{}'", temp_path);
            return false;
        }
        file << json.dump(2);
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, path_, ec);
    if (ec) {
        spdlog::error("Failed to rename temp cron store file: {}", ec.message());
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    return true;
}

bool CronStore::add(const CronJobEntry &entry) {
    auto it = std::ranges::find(jobs_, entry.name, &CronJobEntry::name);
    if (it != jobs_.end()) {
        return false;
    }

    auto updated_jobs = jobs_;
    updated_jobs.push_back(entry);
    if (!save(updated_jobs)) {
        return false;
    }

    jobs_ = std::move(updated_jobs);
    return true;
}

bool CronStore::remove(const std::string &name) {
    auto updated_jobs = jobs_;
    auto it = std::ranges::find(updated_jobs, name, &CronJobEntry::name);
    if (it == updated_jobs.end()) {
        return false;
    }

    updated_jobs.erase(it);
    if (!save(updated_jobs)) {
        return false;
    }

    jobs_ = std::move(updated_jobs);
    return true;
}

const std::vector<CronJobEntry> &CronStore::jobs() const {
    return jobs_;
}

bool CronStore::ensure_directory() const {
    auto dir = std::filesystem::path(path_).parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            spdlog::error("Failed to create cron store directory '{}': {}", dir.string(), ec.message());
            return false;
        }
    }

    return true;
}

} // namespace orangutan
