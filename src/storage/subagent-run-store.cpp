#include "storage/subagent-run-store.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <magic_enum/magic_enum.hpp>

namespace orangutan {

    namespace {

        std::filesystem::path db_path() {
            const auto *home = std::getenv("HOME");
            if (home == nullptr) {
                return std::filesystem::path{"orangutan_sessions.db"};
            }

            return std::filesystem::path(home) / ".orangutan" / "sessions.db";
        }

        SubagentRunStatus status_from_string(std::string_view status) {
            if (const auto parsed = magic_enum::enum_cast<SubagentRunStatus>(status); parsed.has_value()) {
                return *parsed;
            }
            throw std::runtime_error("Unknown subagent run status: " + std::string(status));
        }

        std::optional<std::string> optional_text(std::string value) {
            if (value.empty()) {
                return std::nullopt;
            }
            return value;
        }

    } // namespace

    SubagentRunStore::SubagentRunStore()
    : db_(db_path()) {
        db_.exec("PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
        ensure_schema();
    }

    SubagentRunStore::SubagentRunStore(const std::filesystem::path &db_path)
    : db_(db_path) {
        db_.exec("PRAGMA foreign_keys = ON;", "Failed to enable SQLite foreign keys");
        ensure_schema();
    }

    void SubagentRunStore::ensure_schema() {
        db_.exec(R"(
        CREATE TABLE IF NOT EXISTS subagent_runs (
            run_id TEXT PRIMARY KEY,
            parent_runtime_key TEXT NOT NULL,
            parent_session_id TEXT,
            parent_agent_key TEXT NOT NULL,
            child_session_id TEXT NOT NULL,
            child_agent_key TEXT NOT NULL,
            child_scope_key TEXT NOT NULL,
            status TEXT NOT NULL,
            task_summary TEXT NOT NULL,
            final_summary TEXT NOT NULL DEFAULT '',
            final_output TEXT NOT NULL DEFAULT '',
            error_text TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            started_at TEXT,
            finished_at TEXT,
            FOREIGN KEY (parent_session_id) REFERENCES sessions(id) ON DELETE SET NULL,
            FOREIGN KEY (child_session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_subagent_runs_parent_runtime_status
            ON subagent_runs(parent_runtime_key, status, created_at DESC);
        CREATE INDEX IF NOT EXISTS idx_subagent_runs_parent_session
            ON subagent_runs(parent_session_id, created_at DESC);
        CREATE INDEX IF NOT EXISTS idx_subagent_runs_child_session
            ON subagent_runs(child_session_id);
    )",
                 "Failed to create subagent run schema");
    }

    void SubagentRunStore::create_run(const SubagentRunCreateParams &params) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "INSERT INTO subagent_runs ("
                                    "run_id, parent_runtime_key, parent_session_id, parent_agent_key, "
                                    "child_session_id, child_agent_key, child_scope_key, status, task_summary"
                                    ") VALUES (?, ?, CASE WHEN ? = '' THEN NULL ELSE ? END, ?, ?, ?, ?, ?, ?)");
        stmt.bind_text(1, params.run_id);
        stmt.bind_text(2, params.parent_runtime_key);
        const auto parent_session_id = params.parent_session_id.value_or(std::string{});
        stmt.bind_text(3, parent_session_id);
        stmt.bind_text(4, parent_session_id);
        stmt.bind_text(5, params.parent_agent_key);
        stmt.bind_text(6, params.child_session_id);
        stmt.bind_text(7, params.child_agent_key);
        stmt.bind_text(8, params.child_scope_key);
        stmt.bind_text(9, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(10, params.task_summary);
        static_cast<void>(stmt.step());
    }

    std::optional<SubagentRunRecord> SubagentRunStore::load_run(const std::string &run_id) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "SELECT run_id, parent_runtime_key, COALESCE(parent_session_id, ''), parent_agent_key, "
                                    "child_session_id, child_agent_key, child_scope_key, status, task_summary, "
                                    "final_summary, final_output, error_text, created_at, COALESCE(started_at, ''), COALESCE(finished_at, '') "
                                    "FROM subagent_runs WHERE run_id = ? LIMIT 1");
        stmt.bind_text(1, run_id);
        if (!stmt.step()) {
            return std::nullopt;
        }

        return SubagentRunRecord{
            .run_id = stmt.column_text(0),
            .parent_runtime_key = stmt.column_text(1),
            .parent_session_id = optional_text(stmt.column_text(2)),
            .parent_agent_key = stmt.column_text(3),
            .child_session_id = stmt.column_text(4),
            .child_agent_key = stmt.column_text(5),
            .child_scope_key = stmt.column_text(6),
            .status = status_from_string(stmt.column_text(7)),
            .task_summary = stmt.column_text(8),
            .final_summary = stmt.column_text(9),
            .final_output = stmt.column_text(10),
            .error_text = stmt.column_text(11),
            .created_at = stmt.column_text(12),
            .started_at = optional_text(stmt.column_text(13)),
            .finished_at = optional_text(stmt.column_text(14)),
        };
    }

    void SubagentRunStore::mark_running(const std::string &run_id) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, started_at = COALESCE(started_at, datetime('now')) "
                                    "WHERE run_id = ? AND status = ?");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::running));
        stmt.bind_text(2, run_id);
        stmt.bind_text(3, magic_enum::enum_name(SubagentRunStatus::queued));
        static_cast<void>(stmt.step());
        require_updated_row(run_id, "mark running");
    }

    void SubagentRunStore::mark_succeeded(const std::string &run_id, const std::string &summary, const std::string &output) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, final_summary = ?, final_output = ?, error_text = '', "
                                    "finished_at = datetime('now') WHERE run_id = ? AND status = ?");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::succeeded));
        stmt.bind_text(2, summary);
        stmt.bind_text(3, output);
        stmt.bind_text(4, run_id);
        stmt.bind_text(5, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
        require_updated_row(run_id, "mark succeeded");
    }

    void SubagentRunStore::mark_failed(const std::string &run_id, const std::string &error) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, error_text = ?, finished_at = datetime('now') "
                                    "WHERE run_id = ? AND status IN (?, ?)");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::failed));
        stmt.bind_text(2, error);
        stmt.bind_text(3, run_id);
        stmt.bind_text(4, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(5, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
        require_updated_row(run_id, "mark failed");
    }

    void SubagentRunStore::mark_timed_out(const std::string &run_id, const std::string &error) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, error_text = ?, finished_at = datetime('now') "
                                    "WHERE run_id = ? AND status IN (?, ?)");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::timed_out));
        stmt.bind_text(2, error);
        stmt.bind_text(3, run_id);
        stmt.bind_text(4, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(5, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
        require_updated_row(run_id, "mark timed out");
    }

    void SubagentRunStore::mark_abandoned(const std::string &run_id) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, finished_at = datetime('now') "
                                    "WHERE run_id = ? AND status IN (?, ?)");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::abandoned));
        stmt.bind_text(2, run_id);
        stmt.bind_text(3, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(4, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
        require_updated_row(run_id, "mark abandoned");
    }

    void SubagentRunStore::mark_active_runs_abandoned() {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, finished_at = datetime('now') "
                                    "WHERE status IN (?, ?)");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::abandoned));
        stmt.bind_text(2, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(3, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
    }

    void SubagentRunStore::mark_active_runs_abandoned_for_runtime(const std::string &parent_runtime_key) {
        std::scoped_lock lock(mutex_);

        sqlite::Statement stmt(db_, "UPDATE subagent_runs SET status = ?, finished_at = datetime('now') "
                                    "WHERE parent_runtime_key = ? AND status IN (?, ?)");
        stmt.bind_text(1, magic_enum::enum_name(SubagentRunStatus::abandoned));
        stmt.bind_text(2, parent_runtime_key);
        stmt.bind_text(3, magic_enum::enum_name(SubagentRunStatus::queued));
        stmt.bind_text(4, magic_enum::enum_name(SubagentRunStatus::running));
        static_cast<void>(stmt.step());
    }

    void SubagentRunStore::require_updated_row(const std::string &run_id, const std::string &operation) const {
        if (db_.changes() == 0) {
            throw std::runtime_error("Failed to " + operation + " for unknown subagent run: " + run_id);
        }
    }

} // namespace orangutan
