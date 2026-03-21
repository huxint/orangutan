#include "features/automation/store.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace orangutan::automation {
namespace {

std::string default_db_path() {
    const char *home = std::getenv("HOME");
    if (home == nullptr || std::string_view(home).empty()) {
        return "automation.db";
    }
    return (std::filesystem::path(home) / ".orangutan" / "automation.db").string();
}

std::string encode_optional_seconds(std::optional<std::int64_t> value) {
    return value.has_value() ? std::to_string(*value) : std::string{};
}

std::optional<std::int64_t> decode_optional_seconds(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return std::stoll(std::string(value));
}

bool matches_id_or_name(std::string_view candidate_id, std::string_view candidate_name, std::string_view id_or_name) {
    return candidate_id == id_or_name || candidate_name == id_or_name;
}

TaskSpec read_task(sqlite::Statement &stmt) {
    TaskSpec task;
    task.id = stmt.column_text(0);
    task.agent_key = stmt.column_text(1);
    task.name = stmt.column_text(2);
    task.enabled = stmt.column_int(3) != 0;
    task.schedule.kind = task_schedule_kind_from_string(stmt.column_text(4)).value_or(TaskScheduleKind::cron);
    task.schedule.value = stmt.column_text(5);
    task.prompt = stmt.column_text(6);
    task.notes = stmt.column_text(7);
    task.delivery = delivery_policy_from_json(json::parse(stmt.column_text(8), nullptr, false));
    task.last_run_at = decode_optional_seconds(stmt.column_text(9));
    task.last_status = stmt.column_text(10);
    return task;
}

HeartbeatSpec read_heartbeat(sqlite::Statement &stmt) {
    HeartbeatSpec heartbeat;
    heartbeat.id = stmt.column_text(0);
    heartbeat.agent_key = stmt.column_text(1);
    heartbeat.name = stmt.column_text(2);
    heartbeat.enabled = stmt.column_int(3) != 0;
    heartbeat.every_seconds = stmt.column_int(4);
    heartbeat.jitter_seconds = stmt.column_int(5);
    heartbeat.active_hours = active_hours_from_json(json::parse(stmt.column_text(6), nullptr, false));
    heartbeat.prompt = stmt.column_text(7);
    heartbeat.notes = stmt.column_text(8);
    heartbeat.delivery = delivery_policy_from_json(json::parse(stmt.column_text(9), nullptr, false));
    heartbeat.paused = stmt.column_int(10) != 0;
    heartbeat.next_due_at = decode_optional_seconds(stmt.column_text(11));
    heartbeat.last_run_at = decode_optional_seconds(stmt.column_text(12));
    heartbeat.last_status = stmt.column_text(13);
    return heartbeat;
}

RunRecord read_run(sqlite::Statement &stmt) {
    RunRecord run;
    run.id = stmt.column_text(0);
    run.kind = kind_from_string(stmt.column_text(1)).value_or(Kind::task);
    run.automation_id = stmt.column_text(2);
    run.agent_key = stmt.column_text(3);
    run.automation_name = stmt.column_text(4);
    run.started_at = std::stoll(stmt.column_text(5));
    run.finished_at = decode_optional_seconds(stmt.column_text(6));
    run.status = stmt.column_text(7);
    run.summary = stmt.column_text(8);
    run.delivery_status = stmt.column_text(9);
    run.log_path = stmt.column_text(10);
    return run;
}

InboxItem read_inbox(sqlite::Statement &stmt) {
    InboxItem item;
    item.id = stmt.column_text(0);
    item.agent_key = stmt.column_text(1);
    item.source_kind = stmt.column_text(2);
    item.source_run_id = stmt.column_text(3);
    item.title = stmt.column_text(4);
    item.body = stmt.column_text(5);
    item.created_at = std::stoll(stmt.column_text(6));
    item.acked_at = decode_optional_seconds(stmt.column_text(7));
    item.status = stmt.column_text(8);
    return item;
}

} // namespace

Store::Store()
: db_(default_db_path()) {
    ensure_schema();
}

Store::Store(const std::string &db_path)
: db_(db_path) {
    ensure_schema();
}

std::vector<TaskSpec> Store::list_tasks(const std::string &agent_key) const {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status "
                                "FROM tasks "
                                "WHERE (?1 = '' OR agent_key = ?1) "
                                "ORDER BY agent_key, name");
    stmt.bind_text(1, agent_key);

    std::vector<TaskSpec> tasks;
    while (stmt.step()) {
        tasks.push_back(read_task(stmt));
    }
    return tasks;
}

std::optional<TaskSpec> Store::find_task(const std::string &agent_key, const std::string &id_or_name) const {
    for (const auto &task : list_tasks(agent_key)) {
        if (matches_id_or_name(task.id, task.name, id_or_name)) {
            return task;
        }
    }
    return std::nullopt;
}

std::string Store::upsert_task(const TaskSpec &task_input) {
    std::lock_guard lock(mutex_);
    TaskSpec task = task_input;
    if (task.id.empty()) {
        task.id = generate_id("task");
    }

    sqlite::Statement stmt(db_, "INSERT INTO tasks (id, agent_key, name, enabled, schedule_kind, schedule_value, prompt, notes, delivery_json, last_run_at, last_status) "
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
                                "last_status = excluded.last_status");
    stmt.bind_text(1, task.id);
    stmt.bind_text(2, task.agent_key);
    stmt.bind_text(3, task.name);
    stmt.bind_int(4, task.enabled ? 1 : 0);
    stmt.bind_text(5, task_schedule_kind_to_string(task.schedule.kind));
    stmt.bind_text(6, task.schedule.value);
    stmt.bind_text(7, task.prompt);
    stmt.bind_text(8, task.notes);
    stmt.bind_text(9, delivery_policy_to_json(task.delivery).dump());
    stmt.bind_text(10, encode_optional_seconds(task.last_run_at));
    stmt.bind_text(11, task.last_status);
    (void)stmt.step();
    return task.id;
}

bool Store::remove_task(const std::string &agent_key, const std::string &id_or_name) {
    const auto existing = find_task(agent_key, id_or_name);
    if (!existing.has_value()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "DELETE FROM tasks WHERE id = ?1");
    stmt.bind_text(1, existing->id);
    (void)stmt.step();
    return db_.changes() > 0;
}

void Store::update_task_run_state(const std::string &task_id, std::optional<std::int64_t> last_run_at, std::string_view last_status, bool enabled) {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "UPDATE tasks SET last_run_at = ?2, last_status = ?3, enabled = ?4 WHERE id = ?1");
    stmt.bind_text(1, task_id);
    stmt.bind_text(2, encode_optional_seconds(last_run_at));
    stmt.bind_text(3, std::string(last_status));
    stmt.bind_int(4, enabled ? 1 : 0);
    (void)stmt.step();
}

std::vector<HeartbeatSpec> Store::list_heartbeats(const std::string &agent_key) const {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT id, agent_key, name, enabled, every_seconds, jitter_seconds, active_hours_json, prompt, notes, delivery_json, paused, next_due_at, "
                                "last_run_at, last_status "
                                "FROM heartbeats "
                                "WHERE (?1 = '' OR agent_key = ?1) "
                                "ORDER BY agent_key, name");
    stmt.bind_text(1, agent_key);

    std::vector<HeartbeatSpec> heartbeats;
    while (stmt.step()) {
        heartbeats.push_back(read_heartbeat(stmt));
    }
    return heartbeats;
}

std::optional<HeartbeatSpec> Store::find_heartbeat(const std::string &agent_key, const std::string &id_or_name) const {
    for (const auto &heartbeat : list_heartbeats(agent_key)) {
        if (matches_id_or_name(heartbeat.id, heartbeat.name, id_or_name)) {
            return heartbeat;
        }
    }
    return std::nullopt;
}

std::string Store::upsert_heartbeat(const HeartbeatSpec &heartbeat_input) {
    std::lock_guard lock(mutex_);
    HeartbeatSpec heartbeat = heartbeat_input;
    if (heartbeat.id.empty()) {
        heartbeat.id = generate_id("heartbeat");
    }

    sqlite::Statement stmt(
        db_,
        "INSERT INTO heartbeats (id, agent_key, name, enabled, every_seconds, jitter_seconds, active_hours_json, prompt, notes, delivery_json, paused, next_due_at, last_run_at, "
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
        "last_status = excluded.last_status");
    stmt.bind_text(1, heartbeat.id);
    stmt.bind_text(2, heartbeat.agent_key);
    stmt.bind_text(3, heartbeat.name);
    stmt.bind_int(4, heartbeat.enabled ? 1 : 0);
    stmt.bind_int(5, heartbeat.every_seconds);
    stmt.bind_int(6, heartbeat.jitter_seconds);
    stmt.bind_text(7, active_hours_to_json(heartbeat.active_hours).dump());
    stmt.bind_text(8, heartbeat.prompt);
    stmt.bind_text(9, heartbeat.notes);
    stmt.bind_text(10, delivery_policy_to_json(heartbeat.delivery).dump());
    stmt.bind_int(11, heartbeat.paused ? 1 : 0);
    stmt.bind_text(12, encode_optional_seconds(heartbeat.next_due_at));
    stmt.bind_text(13, encode_optional_seconds(heartbeat.last_run_at));
    stmt.bind_text(14, heartbeat.last_status);
    (void)stmt.step();
    return heartbeat.id;
}

bool Store::remove_heartbeat(const std::string &agent_key, const std::string &id_or_name) {
    const auto existing = find_heartbeat(agent_key, id_or_name);
    if (!existing.has_value()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "DELETE FROM heartbeats WHERE id = ?1");
    stmt.bind_text(1, existing->id);
    (void)stmt.step();
    return db_.changes() > 0;
}

void Store::update_heartbeat_run_state(const std::string &heartbeat_id, std::optional<std::int64_t> last_run_at, std::optional<std::int64_t> next_due_at,
                                       std::string_view last_status, bool paused) {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "UPDATE heartbeats SET last_run_at = ?2, next_due_at = ?3, last_status = ?4, paused = ?5 WHERE id = ?1");
    stmt.bind_text(1, heartbeat_id);
    stmt.bind_text(2, encode_optional_seconds(last_run_at));
    stmt.bind_text(3, encode_optional_seconds(next_due_at));
    stmt.bind_text(4, std::string(last_status));
    stmt.bind_int(5, paused ? 1 : 0);
    (void)stmt.step();
}

std::string Store::insert_run(const RunRecord &run_input) {
    std::lock_guard lock(mutex_);
    RunRecord run = run_input;
    if (run.id.empty()) {
        run.id = generate_id("run");
    }

    sqlite::Statement stmt(db_, "INSERT INTO automation_runs "
                                "(id, kind, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, delivery_status, log_path) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
    stmt.bind_text(1, run.id);
    stmt.bind_text(2, kind_to_string(run.kind));
    stmt.bind_text(3, run.automation_id);
    stmt.bind_text(4, run.agent_key);
    stmt.bind_text(5, run.automation_name);
    stmt.bind_text(6, std::to_string(run.started_at));
    stmt.bind_text(7, encode_optional_seconds(run.finished_at));
    stmt.bind_text(8, run.status);
    stmt.bind_text(9, run.summary);
    stmt.bind_text(10, run.delivery_status);
    stmt.bind_text(11, run.log_path);
    (void)stmt.step();
    return run.id;
}

void Store::complete_run(const std::string &run_id, std::string_view status, std::string_view summary, std::string_view delivery_status, std::string_view log_path,
                         std::optional<std::int64_t> finished_at) {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "UPDATE automation_runs SET finished_at = ?2, status = ?3, summary = ?4, delivery_status = ?5, log_path = ?6 WHERE id = ?1");
    stmt.bind_text(1, run_id);
    stmt.bind_text(2, encode_optional_seconds(finished_at));
    stmt.bind_text(3, std::string(status));
    stmt.bind_text(4, std::string(summary));
    stmt.bind_text(5, std::string(delivery_status));
    stmt.bind_text(6, std::string(log_path));
    (void)stmt.step();
}

std::vector<RunRecord> Store::list_runs(const std::string &agent_key) const {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT id, kind, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, delivery_status, log_path "
                                "FROM automation_runs "
                                "WHERE (?1 = '' OR agent_key = ?1) "
                                "ORDER BY started_at DESC");
    stmt.bind_text(1, agent_key);

    std::vector<RunRecord> runs;
    while (stmt.step()) {
        runs.push_back(read_run(stmt));
    }
    return runs;
}

std::string Store::insert_inbox(const InboxItem &item_input) {
    std::lock_guard lock(mutex_);
    InboxItem item = item_input;
    if (item.id.empty()) {
        item.id = generate_id("inbox");
    }

    sqlite::Statement stmt(db_, "INSERT INTO agent_inbox (id, agent_key, source_kind, source_run_id, title, body, created_at, acked_at, status) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    stmt.bind_text(1, item.id);
    stmt.bind_text(2, item.agent_key);
    stmt.bind_text(3, item.source_kind);
    stmt.bind_text(4, item.source_run_id);
    stmt.bind_text(5, item.title);
    stmt.bind_text(6, item.body);
    stmt.bind_text(7, std::to_string(item.created_at));
    stmt.bind_text(8, encode_optional_seconds(item.acked_at));
    stmt.bind_text(9, item.status);
    (void)stmt.step();
    return item.id;
}

std::vector<InboxItem> Store::list_inbox(const std::string &agent_key) const {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "SELECT id, agent_key, source_kind, source_run_id, title, body, created_at, acked_at, status "
                                "FROM agent_inbox "
                                "WHERE agent_key = ?1 "
                                "ORDER BY created_at DESC");
    stmt.bind_text(1, agent_key);

    std::vector<InboxItem> items;
    while (stmt.step()) {
        items.push_back(read_inbox(stmt));
    }
    return items;
}

bool Store::ack_inbox(const std::string &agent_key, const std::string &id) {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "UPDATE agent_inbox SET acked_at = ?3, status = 'acked' WHERE agent_key = ?1 AND id = ?2");
    stmt.bind_text(1, agent_key);
    stmt.bind_text(2, id);
    stmt.bind_text(3, std::to_string(to_unix_seconds(Clock::now())));
    (void)stmt.step();
    return db_.changes() > 0;
}

void Store::clear_inbox(const std::string &agent_key) {
    std::lock_guard lock(mutex_);
    sqlite::Statement stmt(db_, "DELETE FROM agent_inbox WHERE agent_key = ?1");
    stmt.bind_text(1, agent_key);
    (void)stmt.step();
}

void Store::ensure_schema() {
    db_.exec("CREATE TABLE IF NOT EXISTS tasks ("
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
    db_.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_agent_name ON tasks(agent_key, name);", "create tasks index");

    db_.exec("CREATE TABLE IF NOT EXISTS heartbeats ("
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
    db_.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_heartbeats_agent_name ON heartbeats(agent_key, name);", "create heartbeats index");

    db_.exec("CREATE TABLE IF NOT EXISTS automation_runs ("
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

    db_.exec("CREATE TABLE IF NOT EXISTS agent_inbox ("
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
