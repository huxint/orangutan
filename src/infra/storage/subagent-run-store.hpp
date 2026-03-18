#pragma once

#include "infra/storage/sqlite.hpp"

#include <mutex>
#include <optional>
#include <string>

namespace orangutan {

enum class SubagentRunStatus {
    queued,
    running,
    succeeded,
    failed,
    timed_out,
    abandoned,
};

struct SubagentRunCreateParams {
    std::string run_id;
    std::string parent_runtime_key;
    std::optional<std::string> parent_session_id;
    std::string parent_agent_key;
    std::string child_session_id;
    std::string child_agent_key;
    std::string child_scope_key;
    std::string task_summary;
};

struct SubagentRunRecord {
    std::string run_id;
    std::string parent_runtime_key;
    std::optional<std::string> parent_session_id;
    std::string parent_agent_key;
    std::string child_session_id;
    std::string child_agent_key;
    std::string child_scope_key;
    SubagentRunStatus status = SubagentRunStatus::queued;
    std::string task_summary;
    std::string final_summary;
    std::string final_output;
    std::string error_text;
    std::string created_at;
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
};

class SubagentRunStore {
public:
    SubagentRunStore();
    explicit SubagentRunStore(const std::string &db_path);
    ~SubagentRunStore() = default;

    SubagentRunStore(const SubagentRunStore &) = delete;
    SubagentRunStore &operator=(const SubagentRunStore &) = delete;
    SubagentRunStore(SubagentRunStore &&) = delete;
    SubagentRunStore &operator=(SubagentRunStore &&) = delete;

    void create_run(const SubagentRunCreateParams &params);
    [[nodiscard]]
    std::optional<SubagentRunRecord> load_run(const std::string &run_id);
    void mark_running(const std::string &run_id);
    void mark_succeeded(const std::string &run_id, const std::string &summary, const std::string &output);
    void mark_failed(const std::string &run_id, const std::string &error);
    void mark_timed_out(const std::string &run_id, const std::string &error);
    void mark_abandoned(const std::string &run_id);
    void mark_active_runs_abandoned_for_runtime(const std::string &parent_runtime_key);
    void mark_active_runs_abandoned();

private:
    sqlite::Database db_;
    mutable std::mutex mutex_;

    void ensure_schema();
    void require_updated_row(const std::string &run_id, const std::string &operation) const;
};

} // namespace orangutan
