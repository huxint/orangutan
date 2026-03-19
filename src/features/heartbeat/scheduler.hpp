#pragma once

#include "features/cron/parser.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <cstdint>
#include <vector>

namespace orangutan {

struct HeartbeatJob {
    std::string name;
    CronExpr cron_expr;
    std::string agent;
    std::string channel;
    std::string prompt;
    std::optional<TimePoint> last_fired;
    bool dynamic = false;
};

class HeartbeatScheduler {
public:
    using JobCallback = std::function<void(const HeartbeatJob &job)>;

    explicit HeartbeatScheduler(JobCallback callback);
    ~HeartbeatScheduler();

    HeartbeatScheduler(const HeartbeatScheduler &) = delete;
    HeartbeatScheduler &operator=(const HeartbeatScheduler &) = delete;
    HeartbeatScheduler(HeartbeatScheduler &&) = delete;
    HeartbeatScheduler &operator=(HeartbeatScheduler &&) = delete;

    void add_job(std::string name, CronExpr expr, std::string agent, std::string channel, std::string prompt, bool dynamic = false);

    /// Remove a job by name. Only removes dynamic jobs. Returns true if removed.
    bool remove_job(const std::string &name);

    /// Check if a job with the given name exists.
    [[nodiscard]]
    bool has_job(const std::string &name) const;

    void start();
    void stop();
    void run_pending(TimePoint now);

    void set_heartbeat_md_path(std::string path);

    /// Fire a specific job immediately by name. Returns false if not found.
    bool fire_job(const std::string &name);

    [[nodiscard]]
    std::vector<HeartbeatJob> jobs() const;

private:
    std::vector<HeartbeatJob> jobs_;
    JobCallback callback_;
    std::thread worker_;
    std::string heartbeat_md_path_;
    mutable std::mutex jobs_mutex_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<std::uint64_t> jobs_revision_{0};
    std::atomic<bool> running_{false};

    void scheduler_loop();
};

} // namespace orangutan
