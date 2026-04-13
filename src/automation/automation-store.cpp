#include "automation/automation-store.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>

#include <magic_enum/magic_enum.hpp>
#include <stdexcept>

namespace orangutan::automation {
    namespace {

        std::filesystem::path default_db_path() {
            const char *home = std::getenv("HOME");
            if (home == nullptr || std::string_view(home).empty()) {
                return std::filesystem::path{"automation.db"};
            }
            return std::filesystem::path(home) / ".orangutan" / "automation.db";
        }

        std::string encode_optional_seconds(std::optional<base::i64> value) {
            return value.has_value() ? std::to_string(*value) : std::string{};
        }

        std::optional<base::i64> decode_optional_seconds(std::string_view value) {
            if (value.empty()) {
                return std::nullopt;
            }
            base::i64 result{};
            std::from_chars(value.begin(), value.end(), result);
            return result;
        }

        using task_row = std::tuple<std::string, std::string, std::string, int, std::string, std::string, std::string, std::string, std::string, std::string, std::string>;
        using heartbeat_row =
            std::tuple<std::string, std::string, std::string, int, int, int, std::string, std::string, std::string, std::string, int, std::string, std::string, std::string>;
        using run_row =
            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string>;
        using inbox_row = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string>;

        auto read_task(const task_row &row) -> TaskSpec {
            TaskSpec task;
            task.id = std::get<0>(row);
            task.agent_key = std::get<1>(row);
            task.name = std::get<2>(row);
            task.enabled = std::get<3>(row) != 0;
            const auto &schedule_kind = std::get<4>(row);
            if (const auto parsed = magic_enum::enum_cast<task_schedule_kind>(schedule_kind); parsed.has_value()) {
                task.schedule.kind = *parsed;
            } else {
                throw std::runtime_error("Unknown task schedule kind: " + schedule_kind);
            }
            task.schedule.value = std::get<5>(row);
            task.prompt = std::get<6>(row);
            task.notes = std::get<7>(row);
            task.delivery = delivery_policy_from_json(nlohmann::json::parse(std::get<8>(row), nullptr, false));
            task.last_run_at = decode_optional_seconds(std::get<9>(row));
            task.last_status = std::get<10>(row);
            return task;
        }

        auto read_heartbeat(const heartbeat_row &row) -> HeartbeatSpec {
            HeartbeatSpec heartbeat;
            heartbeat.id = std::get<0>(row);
            heartbeat.agent_key = std::get<1>(row);
            heartbeat.name = std::get<2>(row);
            heartbeat.enabled = std::get<3>(row) != 0;
            heartbeat.every_seconds = std::get<4>(row);
            heartbeat.jitter_seconds = std::get<5>(row);
            heartbeat.active_hours = active_hours_from_json(nlohmann::json::parse(std::get<6>(row), nullptr, false));
            heartbeat.prompt = std::get<7>(row);
            heartbeat.notes = std::get<8>(row);
            heartbeat.delivery = delivery_policy_from_json(nlohmann::json::parse(std::get<9>(row), nullptr, false));
            heartbeat.paused = std::get<10>(row) != 0;
            heartbeat.next_due_at = decode_optional_seconds(std::get<11>(row));
            heartbeat.last_run_at = decode_optional_seconds(std::get<12>(row));
            heartbeat.last_status = std::get<13>(row);
            return heartbeat;
        }

        auto read_run(const run_row &row) -> RunRecord {
            RunRecord run;
            run.id = std::get<0>(row);
            const auto &kind = std::get<1>(row);
            if (const auto parsed = magic_enum::enum_cast<automation::kind>(kind); parsed.has_value()) {
                run.kind = *parsed;
            } else {
                throw std::runtime_error("Unknown automation kind: " + kind);
            }
            run.automation_id = std::get<2>(row);
            run.agent_key = std::get<3>(row);
            run.automation_name = std::get<4>(row);
            const auto &started_text = std::get<5>(row);
            static_cast<void>(std::from_chars(started_text.data(), started_text.data() + started_text.size(), run.started_at));
            run.finished_at = decode_optional_seconds(std::get<6>(row));
            run.status = std::get<7>(row);
            run.summary = std::get<8>(row);
            run.delivery_status = std::get<9>(row);
            run.log_path = std::get<10>(row);
            return run;
        }

        auto read_inbox(const inbox_row &row) -> InboxItem {
            InboxItem item;
            item.id = std::get<0>(row);
            item.agent_key = std::get<1>(row);
            item.source_kind = std::get<2>(row);
            item.source_run_id = std::get<3>(row);
            item.title = std::get<4>(row);
            item.body = std::get<5>(row);
            const auto &created_text = std::get<6>(row);
            static_cast<void>(std::from_chars(created_text.data(), created_text.data() + created_text.size(), item.created_at));
            item.acked_at = decode_optional_seconds(std::get<7>(row));
            item.status = std::get<8>(row);
            return item;
        }

    } // namespace

    Store::Store()
    : db_(default_db_path()) {
        ensure_schema();
    }

    Store::Store(const std::filesystem::path &db_path)
    : db_(db_path) {
        ensure_schema();
    }

    std::vector<TaskSpec> Store::list_tasks(std::string_view agent_key) const {
        std::scoped_lock lock(mutex_);
        std::vector<TaskSpec> tasks;
        for (const auto &row : db_.query("SELECT id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status "
                                         "FROM tasks "
                                         "WHERE (?1 = '' OR agent_key = ?1) "
                                         "ORDER BY agent_key, name")
                                   .bind(agent_key)
                                   .all<task_row>()) {
            tasks.push_back(read_task(row));
        }
        return tasks;
    }

    std::optional<TaskSpec> Store::find_task(std::string_view agent_key, std::string_view id_or_name) const {
        std::scoped_lock lock(mutex_);
        if (const auto row = db_.query("SELECT id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status "
                                       "FROM tasks "
                                       "WHERE (?1 = '' OR agent_key = ?1) AND (id = ?2 OR name = ?2) "
                                       "LIMIT 1")
                                 .bind(agent_key, id_or_name)
                                 .optional<task_row>();
            row.has_value()) {
            return read_task(*row);
        }
        return std::nullopt;
    }

    std::string Store::upsert_task(const TaskSpec &task_input) {
        std::scoped_lock lock(mutex_);
        TaskSpec task = task_input;
        if (task.id.empty()) {
            task.id = generate_id("task");
        }

        db_.exec("INSERT INTO tasks (id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
                 "ON CONFLICT(id) DO UPDATE SET "
                 "agent_key = excluded.agent_key, "
                 "name = excluded.name, "
                 "enabled = excluded.enabled, "
                 "schedule_kind = excluded.schedule_kind, "
                 "schedule_value = excluded.schedule_value, "
                 "prompt = excluded.prompt, "
                 "notes = excluded.notes, "
                 "delivery_json = excluded.delivery_json, "
                 "last_run_at = excluded.last_run_at, "
                 "last_status = excluded.last_status")
            .bind(task.id,
                  task.agent_key,
                  task.name,
                  task.enabled ? 1 : 0,
                  magic_enum::enum_name(task.schedule.kind),
                  task.schedule.value,
                  task.prompt,
                  task.notes,
                  delivery_policy_to_json(task.delivery).dump(),
                  encode_optional_seconds(task.last_run_at),
                  task.last_status)
            .run();
        return task.id;
    }

    bool Store::remove_task(std::string_view agent_key, std::string_view id_or_name) {
        std::scoped_lock lock(mutex_);
        db_.exec("DELETE FROM tasks WHERE (?1 = '' OR agent_key = ?1) AND (id = ?2 OR name = ?2)").bind(agent_key, id_or_name).run();
        return db_.changes() > 0;
    }

    void Store::update_task_run_state(std::string_view task_id, std::optional<base::i64> last_run_at, std::string_view last_status, bool enabled) {
        std::scoped_lock lock(mutex_);
        db_.exec("UPDATE tasks SET last_run_at = ?2, last_status = ?3, enabled = ?4 WHERE id = ?1")
            .bind(task_id, encode_optional_seconds(last_run_at), last_status, enabled ? 1 : 0)
            .run();
    }

    std::vector<HeartbeatSpec> Store::list_heartbeats(std::string_view agent_key) const {
        std::scoped_lock lock(mutex_);
        std::vector<HeartbeatSpec> heartbeats;
        for (const auto &row : db_.query("SELECT id, agent_key, name, enabled, every_seconds, jitter_seconds, active_hours_json, prompt, notes, delivery_json, paused, next_due_at, "
                                         "last_run_at, last_status "
                                         "FROM heartbeats "
                                         "WHERE (?1 = '' OR agent_key = ?1) "
                                         "ORDER BY agent_key, name")
                                   .bind(agent_key)
                                   .all<heartbeat_row>()) {
            heartbeats.push_back(read_heartbeat(row));
        }
        return heartbeats;
    }

    std::optional<HeartbeatSpec> Store::find_heartbeat(std::string_view agent_key, std::string_view id_or_name) const {
        std::scoped_lock lock(mutex_);
        if (const auto row = db_.query("SELECT id, agent_key, name, enabled, every_seconds, jitter_seconds, active_hours_json, prompt, notes, delivery_json, paused, next_due_at, "
                                       "last_run_at, last_status "
                                       "FROM heartbeats "
                                       "WHERE (?1 = '' OR agent_key = ?1) AND (id = ?2 OR name = ?2) "
                                       "LIMIT 1")
                                 .bind(agent_key, id_or_name)
                                 .optional<heartbeat_row>();
            row.has_value()) {
            return read_heartbeat(*row);
        }
        return std::nullopt;
    }

    std::string Store::upsert_heartbeat(const HeartbeatSpec &heartbeat_input) {
        std::scoped_lock lock(mutex_);
        HeartbeatSpec heartbeat = heartbeat_input;
        if (heartbeat.id.empty()) {
            heartbeat.id = generate_id("heartbeat");
        }

        db_.exec("INSERT INTO heartbeats (id, agent_key, name, enabled, every_seconds, jitter_seconds, active_hours_json, prompt, notes, delivery_json, paused, "
                 "next_due_at, last_run_at, "
                 "last_status) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14) "
                 "ON CONFLICT(id) DO UPDATE SET "
                 "agent_key = excluded.agent_key, "
                 "name = excluded.name, "
                 "enabled = excluded.enabled, "
                 "every_seconds = excluded.every_seconds, "
                 "jitter_seconds = excluded.jitter_seconds, "
                 "active_hours_json = excluded.active_hours_json, "
                 "prompt = excluded.prompt, "
                 "notes = excluded.notes, "
                 "delivery_json = excluded.delivery_json, "
                 "paused = excluded.paused, "
                 "next_due_at = excluded.next_due_at, "
                 "last_run_at = excluded.last_run_at, "
                 "last_status = excluded.last_status")
            .bind(heartbeat.id,
                  heartbeat.agent_key,
                  heartbeat.name,
                  heartbeat.enabled ? 1 : 0,
                  heartbeat.every_seconds,
                  heartbeat.jitter_seconds,
                  active_hours_to_json(heartbeat.active_hours).dump(),
                  heartbeat.prompt,
                  heartbeat.notes,
                  delivery_policy_to_json(heartbeat.delivery).dump(),
                  heartbeat.paused ? 1 : 0,
                  encode_optional_seconds(heartbeat.next_due_at),
                  encode_optional_seconds(heartbeat.last_run_at),
                  heartbeat.last_status)
            .run();
        return heartbeat.id;
    }

    bool Store::remove_heartbeat(std::string_view agent_key, std::string_view id_or_name) {
        std::scoped_lock lock(mutex_);
        db_.exec("DELETE FROM heartbeats WHERE (?1 = '' OR agent_key = ?1) AND (id = ?2 OR name = ?2)").bind(agent_key, id_or_name).run();
        return db_.changes() > 0;
    }

    void Store::update_heartbeat_run_state(std::string_view heartbeat_id, std::optional<base::i64> last_run_at, std::optional<base::i64> next_due_at, std::string_view last_status,
                                           bool paused) {
        std::scoped_lock lock(mutex_);
        db_.exec("UPDATE heartbeats SET last_run_at = ?2, next_due_at = ?3, last_status = ?4, paused = ?5 WHERE id = ?1")
            .bind(heartbeat_id, encode_optional_seconds(last_run_at), encode_optional_seconds(next_due_at), last_status, paused ? 1 : 0)
            .run();
    }

    std::string Store::insert_run(const RunRecord &run_input) {
        std::scoped_lock lock(mutex_);
        RunRecord run = run_input;
        if (run.id.empty()) {
            run.id = generate_id("run");
        }

        db_.exec("INSERT INTO automation_runs "
                 "(id, kind, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, delivery_status, log_path) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)")
            .bind(run.id,
                  magic_enum::enum_name(run.kind),
                  run.automation_id,
                  run.agent_key,
                  run.automation_name,
                  std::to_string(run.started_at),
                  encode_optional_seconds(run.finished_at),
                  run.status,
                  run.summary,
                  run.delivery_status,
                  run.log_path)
            .run();
        return run.id;
    }

    void Store::complete_run(std::string_view run_id, std::string_view status, std::string_view summary, std::string_view delivery_status, std::string_view log_path,
                             std::optional<base::i64> finished_at) {
        std::scoped_lock lock(mutex_);
        db_.exec("UPDATE automation_runs SET finished_at = ?2, status = ?3, summary = ?4, delivery_status = ?5, log_path = ?6 WHERE id = ?1")
            .bind(run_id, encode_optional_seconds(finished_at), status, summary, delivery_status, log_path)
            .run();
    }

    std::vector<RunRecord> Store::list_runs(std::string_view agent_key) const {
        std::scoped_lock lock(mutex_);
        std::vector<RunRecord> runs;
        for (const auto &row : db_.query("SELECT id, kind, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, delivery_status, log_path "
                                         "FROM automation_runs "
                                         "WHERE (?1 = '' OR agent_key = ?1) "
                                         "ORDER BY started_at DESC")
                                   .bind(agent_key)
                                   .all<run_row>()) {
            runs.push_back(read_run(row));
        }
        return runs;
    }

    std::string Store::insert_inbox(const InboxItem &item_input) {
        std::scoped_lock lock(mutex_);
        InboxItem item = item_input;
        if (item.id.empty()) {
            item.id = generate_id("inbox");
        }

        db_.exec("INSERT INTO agent_inbox (id, agent_key, source_kind, source_run_id, title, body, created_at, acked_at, status) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)")
            .bind(item.id,
                  item.agent_key,
                  item.source_kind,
                  item.source_run_id,
                  item.title,
                  item.body,
                  std::to_string(item.created_at),
                  encode_optional_seconds(item.acked_at),
                  item.status)
            .run();
        return item.id;
    }

    std::vector<InboxItem> Store::list_inbox(std::string_view agent_key) const {
        std::scoped_lock lock(mutex_);
        std::vector<InboxItem> items;
        for (const auto &row : db_.query("SELECT id, agent_key, source_kind, source_run_id, title, body, created_at, acked_at, status "
                                         "FROM agent_inbox "
                                         "WHERE agent_key = ?1 "
                                         "ORDER BY created_at DESC")
                                   .bind(agent_key)
                                   .all<inbox_row>()) {
            items.push_back(read_inbox(row));
        }
        return items;
    }

    bool Store::ack_inbox(std::string_view agent_key, std::string_view id) {
        std::scoped_lock lock(mutex_);
        db_.exec("UPDATE agent_inbox SET acked_at = ?3, status = 'acked' WHERE agent_key = ?1 AND id = ?2")
            .bind(agent_key, id, std::to_string(to_unix_seconds(Clock::now())))
            .run();
        return db_.changes() > 0;
    }

    void Store::clear_inbox(std::string_view agent_key) {
        std::scoped_lock lock(mutex_);
        db_.exec("DELETE FROM agent_inbox WHERE agent_key = ?1").bind(agent_key).run();
    }

    void Store::ensure_schema() {
        db_.exec_script("CREATE TABLE IF NOT EXISTS tasks ("
                        "  id TEXT PRIMARY KEY,"
                        "  agent_key TEXT NOT NULL,"
                        "  name TEXT NOT NULL,"
                        "  enabled INTEGER NOT NULL,"
                        "  schedule_kind TEXT NOT NULL,"
                        "  schedule_value TEXT NOT NULL,"
                        "  prompt TEXT NOT NULL,"
                        "  notes TEXT NOT NULL DEFAULT '',"
                        "  delivery_json TEXT NOT NULL,"
                        "  last_run_at TEXT NOT NULL DEFAULT '',"
                        "  last_status TEXT NOT NULL DEFAULT ''"
                        ");",
                        "create tasks table");
        db_.exec_script("CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_agent_name ON tasks(agent_key, name);", "create tasks index");

        db_.exec_script("CREATE TABLE IF NOT EXISTS heartbeats ("
                        "  id TEXT PRIMARY KEY,"
                        "  agent_key TEXT NOT NULL,"
                        "  name TEXT NOT NULL,"
                        "  enabled INTEGER NOT NULL,"
                        "  every_seconds INTEGER NOT NULL,"
                        "  jitter_seconds INTEGER NOT NULL,"
                        "  active_hours_json TEXT NOT NULL,"
                        "  prompt TEXT NOT NULL,"
                        "  notes TEXT NOT NULL DEFAULT '',"
                        "  delivery_json TEXT NOT NULL,"
                        "  paused INTEGER NOT NULL DEFAULT 0,"
                        "  next_due_at TEXT NOT NULL DEFAULT '',"
                        "  last_run_at TEXT NOT NULL DEFAULT '',"
                        "  last_status TEXT NOT NULL DEFAULT ''"
                        ");",
                        "create heartbeats table");
        db_.exec_script("CREATE UNIQUE INDEX IF NOT EXISTS idx_heartbeats_agent_name ON heartbeats(agent_key, name);", "create heartbeats index");

        db_.exec_script("CREATE TABLE IF NOT EXISTS automation_runs ("
                        "  id TEXT PRIMARY KEY,"
                        "  kind TEXT NOT NULL,"
                        "  automation_id TEXT NOT NULL,"
                        "  agent_key TEXT NOT NULL,"
                        "  automation_name TEXT NOT NULL,"
                        "  started_at TEXT NOT NULL,"
                        "  finished_at TEXT NOT NULL DEFAULT '',"
                        "  status TEXT NOT NULL,"
                        "  summary TEXT NOT NULL DEFAULT '',"
                        "  delivery_status TEXT NOT NULL DEFAULT '',"
                        "  log_path TEXT NOT NULL DEFAULT ''"
                        ");",
                        "create automation_runs table");

        db_.exec_script("CREATE TABLE IF NOT EXISTS agent_inbox ("
                        "  id TEXT PRIMARY KEY,"
                        "  agent_key TEXT NOT NULL,"
                        "  source_kind TEXT NOT NULL,"
                        "  source_run_id TEXT NOT NULL DEFAULT '',"
                        "  title TEXT NOT NULL,"
                        "  body TEXT NOT NULL,"
                        "  created_at TEXT NOT NULL,"
                        "  acked_at TEXT NOT NULL DEFAULT '',"
                        "  status TEXT NOT NULL DEFAULT 'unread'"
                        ");",
                        "create agent_inbox table");
    }

} // namespace orangutan::automation
