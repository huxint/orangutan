#include "features/heartbeat/scheduler.hpp"
#include "features/heartbeat/protocol/heartbeat-md.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace orangutan {

HeartbeatScheduler::HeartbeatScheduler(JobCallback callback)
: callback_(std::move(callback)) {}

HeartbeatScheduler::~HeartbeatScheduler() {
    stop();
}

void HeartbeatScheduler::add_job(std::string name, CronExpr expr, std::string agent, std::string channel, std::string prompt, bool dynamic) {
    {
        std::scoped_lock lock(jobs_mutex_);
        jobs_.push_back(HeartbeatJob{
            .name = std::move(name),
            .cron_expr = std::move(expr),
            .agent = std::move(agent),
            .channel = std::move(channel),
            .prompt = std::move(prompt),
            .last_fired = std::nullopt,
            .dynamic = dynamic,
        });
    }

    jobs_revision_.fetch_add(1);
    cv_.notify_all();
}

bool HeartbeatScheduler::remove_job(const std::string &name) {
    {
        std::scoped_lock lock(jobs_mutex_);
        auto it = std::ranges::find(jobs_, name, &HeartbeatJob::name);
        if (it == jobs_.end()) {
            return false;
        }
        if (!it->dynamic) {
            return false;
        }
        jobs_.erase(it);
    }

    jobs_revision_.fetch_add(1);
    cv_.notify_all();
    return true;
}

bool HeartbeatScheduler::has_job(const std::string &name) const {
    std::scoped_lock lock(jobs_mutex_);
    return std::ranges::contains(jobs_, name, &HeartbeatJob::name);
}

void HeartbeatScheduler::set_heartbeat_md_path(std::string path) {
    std::scoped_lock lock(jobs_mutex_);
    heartbeat_md_path_ = std::move(path);
}

bool HeartbeatScheduler::fire_job(const std::string &name) {
    HeartbeatJob job_copy;
    {
        std::scoped_lock lock(jobs_mutex_);
        auto it = std::ranges::find(jobs_, name, &HeartbeatJob::name);
        if (it == jobs_.end()) {
            return false;
        }
        job_copy = *it;
    }

    // Prepend HEARTBEAT.md content if configured
    auto md_content = load_heartbeat_md(heartbeat_md_path_);
    if (md_content.has_value() && !md_content->empty()) {
        job_copy.prompt = *md_content + "\n\n" + job_copy.prompt;
    }

    try {
        callback_(job_copy);
    } catch (const std::exception &e) {
        spdlog::error("Manual fire of job '{}' failed: {}", name, e.what());
    }
    return true;
}

void HeartbeatScheduler::start() {
    std::size_t job_count = 0;
    {
        std::scoped_lock lock(jobs_mutex_);
        job_count = jobs_.size();
    }

    if (job_count == 0) {
        spdlog::debug("No heartbeat jobs configured, scheduler not started");
        return;
    }

    {
        std::scoped_lock lock(mutex_);
        if (running_.load()) {
            spdlog::debug("Heartbeat scheduler already running, start() is a no-op");
            return;
        }

        running_.store(true);
        worker_ = std::thread([this] {
            scheduler_loop();
        });
    }

    spdlog::info("Heartbeat scheduler started with {} job(s)", job_count);
}

void HeartbeatScheduler::stop() {
    std::thread worker;
    bool was_running = false;

    {
        std::scoped_lock lock(mutex_);
        was_running = running_.exchange(false);
        if (!was_running && !worker_.joinable()) {
            return;
        }

        worker = std::move(worker_);
    }

    if (!worker.joinable()) {
        return;
    }

    cv_.notify_all();
    worker.join();

    if (was_running) {
        spdlog::info("Heartbeat scheduler stopped");
    }
}

void HeartbeatScheduler::run_pending(TimePoint now) {
    const auto current_minute = std::chrono::floor<std::chrono::minutes>(now);
    std::vector<HeartbeatJob> jobs_to_fire;
    std::string md_path;

    {
        std::scoped_lock lock(jobs_mutex_);
        md_path = heartbeat_md_path_;

        for (auto &job : jobs_) {
            if (!cron_matches(job.cron_expr, current_minute)) {
                continue;
            }

            if (job.last_fired == current_minute) {
                continue;
            }

            spdlog::info("Heartbeat firing job '{}'", job.name);
            job.last_fired = current_minute;
            jobs_to_fire.push_back(job);
        }
    }

    // Load HEARTBEAT.md once per run_pending cycle
    auto md_content = load_heartbeat_md(md_path);
    if (md_content.has_value() && md_content->empty()) {
        // File exists but is empty — skip all heartbeat runs this cycle
        spdlog::debug("HEARTBEAT.md is empty, skipping heartbeat runs");
        return;
    }

    for (auto job : jobs_to_fire) {
        // Prepend HEARTBEAT.md content to prompt if available
        if (md_content.has_value() && !md_content->empty()) {
            job.prompt = *md_content + "\n\n" + job.prompt;
        }

        try {
            callback_(job);
        } catch (const std::exception &e) {
            spdlog::error("Heartbeat job '{}' callback failed: {}", job.name, e.what());
        }
    }
}

std::vector<HeartbeatJob> HeartbeatScheduler::jobs() const {
    std::scoped_lock lock(jobs_mutex_);
    return jobs_;
}

void HeartbeatScheduler::scheduler_loop() {
    while (running_.load()) {
        auto now = std::chrono::system_clock::now();
        const auto observed_jobs_revision = jobs_revision_.load();

        run_pending(now);

        // Sleep until the start of the next minute (or until stopped)
        auto next_minute = std::chrono::ceil<std::chrono::minutes>(now + std::chrono::seconds(1));
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next_minute, [this, observed_jobs_revision] {
            return !running_.load() || jobs_revision_.load() != observed_jobs_revision;
        });
    }
}

} // namespace orangutan
