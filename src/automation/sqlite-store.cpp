#include "automation/sqlite-store.hpp"

#include "storage/sqlite.hpp"
#include "utils/enum-string.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace orangutan::automation {

    namespace {

        using load_job_row = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::int64_t, int, int,
                                        std::optional<std::int64_t>, std::optional<std::int64_t>, std::optional<std::int64_t>, std::optional<std::int64_t>, std::string, int,
                                        std::string, std::optional<std::int64_t>, std::int64_t>;

        [[nodiscard]]
        auto current_unix_seconds() -> std::int64_t {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        void validate_non_blank(std::string_view value, std::string_view label) {
            if (value.empty()) {
                throw std::invalid_argument(std::string(label) + " must not be blank");
            }
        }

        [[nodiscard]]
        auto open_store_db(const std::filesystem::path &db_path) -> sqlite::Database {
            auto db = sqlite::Database::create(db_path);
            if (!db) {
                throw std::runtime_error("sqlite: " + db.error().message);
            }
            return std::move(*db);
        }

        template <typename... Args>
        [[nodiscard]]
        auto run_bound(sqlite::Database &db, std::string_view sql, Args &&...args) -> StoreResult<void> {
            auto command = db.exec(sql);
            if (!command) {
                return std::unexpected(command.error());
            }
            auto result = command->bind(std::forward<Args>(args)...).run();
            if (!result) {
                return std::unexpected(result.error());
            }
            return {};
        }

        [[nodiscard]]
        auto parse_json_text(std::string_view text, std::string_view label) -> StoreResult<nlohmann::json> {
            const auto parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded()) {
                return std::unexpected(sqlite::make_error(sqlite::sqlite_error_kind::mapping_error, std::string(label) + " contains invalid json"));
            }
            return parsed;
        }

        [[nodiscard]]
        auto schedule_kind_name(const ScheduleSpec &schedule) -> std::string {
            return std::visit(
                [](const auto &spec) -> std::string {
                    using T = std::decay_t<decltype(spec)>;
                    if constexpr (std::is_same_v<T, CronSchedule>) {
                        return "cron";
                    } else if constexpr (std::is_same_v<T, IntervalSchedule>) {
                        return "interval";
                    } else {
                        return "one_shot";
                    }
                },
                schedule);
        }

        [[nodiscard]]
        auto schedule_to_json(const ScheduleSpec &schedule) -> nlohmann::json {
            return std::visit(
                [](const auto &spec) -> nlohmann::json {
                    using T = std::decay_t<decltype(spec)>;
                    if constexpr (std::is_same_v<T, CronSchedule>) {
                        return {
                            {"expr", spec.expr},
                            {"time_zone", spec.time_zone},
                        };
                    } else if constexpr (std::is_same_v<T, IntervalSchedule>) {
                        nlohmann::json active_windows = nlohmann::json::array();
                        for (const auto &window : spec.active_windows) {
                            active_windows.push_back({
                                {"start_minutes", window.start.count()},
                                {"end_minutes", window.end.count()},
                            });
                        }
                        return {
                            {"every_seconds", spec.every.count()},
                            {"jitter_seconds", spec.jitter.count()},
                            {"time_zone", spec.time_zone},
                            {"active_windows", std::move(active_windows)},
                        };
                    } else {
                        return {
                            {"at_unix_seconds", to_unix_seconds(spec.at)},
                        };
                    }
                },
                schedule);
        }

        [[nodiscard]]
        auto schedule_from_json(std::string_view kind, const nlohmann::json &json) -> StoreResult<ScheduleSpec> {
            if (kind == "cron") {
                return ScheduleSpec{CronSchedule{
                    .expr = json.value("expr", ""),
                    .time_zone = json.value("time_zone", "UTC"),
                }};
            }
            if (kind == "interval") {
                IntervalSchedule schedule{
                    .every = std::chrono::seconds(json.value("every_seconds", 0)),
                    .jitter = std::chrono::seconds(json.value("jitter_seconds", 0)),
                    .active_windows = {},
                    .time_zone = json.value("time_zone", "UTC"),
                };

                if (const auto windows = json.find("active_windows"); windows != json.end() && windows->is_array()) {
                    for (const auto &window : *windows) {
                        schedule.active_windows.push_back(ActiveWindow{
                            .start = std::chrono::minutes(window.value("start_minutes", 0)),
                            .end = std::chrono::minutes(window.value("end_minutes", 0)),
                        });
                    }
                }

                return ScheduleSpec{std::move(schedule)};
            }
            if (kind == "one_shot") {
                return ScheduleSpec{OneShotSchedule{
                    .at = from_unix_seconds(json.value("at_unix_seconds", 0)),
                }};
            }

            return std::unexpected(sqlite::make_error(sqlite::sqlite_error_kind::mapping_error, "unknown schedule kind: " + std::string(kind)));
        }

        [[nodiscard]]
        auto execution_policy_to_json(const ExecutionPolicy &policy) -> nlohmann::json {
            return {
                {"missed_runs", utils::enum_name(policy.missed_runs)},
                {"max_retry_attempts", policy.max_retry_attempts},
                {"initial_backoff_ms", policy.initial_backoff.count()},
                {"max_backoff_ms", policy.max_backoff.count()},
                {"allow_parallel", policy.allow_parallel},
                {"overlap", utils::enum_name(policy.overlap)},
            };
        }

        [[nodiscard]]
        auto execution_policy_from_json(const nlohmann::json &json) -> ExecutionPolicy {
            return ExecutionPolicy{
                .missed_runs = utils::parse_enum_or<MissedRunPolicy>(json.value("missed_runs", "run_once"), MissedRunPolicy::run_once),
                .max_retry_attempts = json.value("max_retry_attempts", 0),
                .initial_backoff = std::chrono::milliseconds(json.value("initial_backoff_ms", 0)),
                .max_backoff = std::chrono::milliseconds(json.value("max_backoff_ms", 0)),
                .allow_parallel = json.value("allow_parallel", false),
                .overlap = utils::parse_enum_or<OverlapPolicy>(json.value("overlap", "forbid"), OverlapPolicy::forbid),
            };
        }

        [[nodiscard]]
        auto result_policy_to_json(const ResultPolicy &policy) -> nlohmann::json {
            return {
                {"mode", utils::enum_name(policy.mode)},
                {"targets", policy.targets},
                {"persist_full_reply", policy.persist_full_reply},
            };
        }

        [[nodiscard]]
        auto result_policy_from_json(const nlohmann::json &json) -> ResultPolicy {
            return ResultPolicy{
                .mode = utils::parse_enum_or<delivery_mode>(json.value("mode", "silent"), delivery_mode::silent),
                .targets = json.value("targets", std::vector<std::string>{}),
                .persist_full_reply = json.value("persist_full_reply", true),
            };
        }

        [[nodiscard]]
        auto decode_stored_job(const load_job_row &row) -> StoreResult<StoredJob> {
            const auto &[job_id, job_key, schedule_kind, schedule_json_text, action_key, action_payload_json_text, execution_policy_json_text, result_policy_json_text,
                          metadata_json_text, version, enabled, paused, next_due_at, last_scheduled_at, last_started_at, last_finished_at, last_status, in_flight_count,
                          lease_owner, lease_expires_at, revision] = row;

            auto schedule_json = parse_json_text(schedule_json_text, "schedule_json");
            if (!schedule_json) {
                return std::unexpected(schedule_json.error());
            }
            auto schedule = schedule_from_json(schedule_kind, *schedule_json);
            if (!schedule) {
                return std::unexpected(schedule.error());
            }

            auto action_payload_json = parse_json_text(action_payload_json_text, "action_payload_json");
            if (!action_payload_json) {
                return std::unexpected(action_payload_json.error());
            }

            auto execution_policy_json = parse_json_text(execution_policy_json_text, "execution_policy_json");
            if (!execution_policy_json) {
                return std::unexpected(execution_policy_json.error());
            }

            auto result_policy_json = parse_json_text(result_policy_json_text, "result_policy_json");
            if (!result_policy_json) {
                return std::unexpected(result_policy_json.error());
            }

            auto metadata_json = parse_json_text(metadata_json_text, "metadata_json");
            if (!metadata_json) {
                return std::unexpected(metadata_json.error());
            }

            return StoredJob{
                .definition =
                    JobDefinition{
                        .id = JobId{.value = job_id},
                        .key = job_key,
                        .schedule = std::move(*schedule),
                        .action =
                            ActionDescriptor{
                                .action_key = action_key,
                                .payload = std::move(*action_payload_json),
                            },
                        .execution = execution_policy_from_json(*execution_policy_json),
                        .result = result_policy_from_json(*result_policy_json),
                        .metadata = std::move(*metadata_json),
                        .version = version,
                    },
                .state =
                    ScheduleState{
                        .enabled = enabled != 0,
                        .paused = paused != 0,
                        .next_due_at = next_due_at,
                        .last_scheduled_at = last_scheduled_at,
                        .last_started_at = last_started_at,
                        .last_finished_at = last_finished_at,
                        .last_status = last_status,
                        .in_flight_count = in_flight_count,
                        .lease_owner = lease_owner,
                        .lease_expires_at = lease_expires_at,
                        .revision = revision,
                    },
            };
        }

    } // namespace

    SqliteJobStore::SqliteJobStore(const std::filesystem::path &db_path)
    : db_(open_store_db(db_path)) {
        const auto schema_result = ensure_schema();
        if (!schema_result) {
            throw std::runtime_error("sqlite: " + schema_result.error().message);
        }
    }

    auto SqliteJobStore::save_job(const JobDefinition &definition, const ScheduleState &state) -> StoreResult<void> {
        validate_non_blank(definition.id.value, "job id");
        validate_non_blank(definition.key, "job key");
        validate_non_blank(definition.action.action_key, "action key");

        std::scoped_lock lock(mutex_);
        const auto now = current_unix_seconds();

        return db_.transaction([&](sqlite::Database &tx) -> StoreResult<void> {
            auto save_definition = run_bound(
                tx,
                "INSERT INTO automation_job_definitions ("
                "job_id, job_key, schedule_kind, schedule_json, action_key, action_payload_json, execution_policy_json, result_policy_json, metadata_json, version, created_at, updated_at"
                ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
                "ON CONFLICT(job_id) DO UPDATE SET "
                "job_key = excluded.job_key, "
                "schedule_kind = excluded.schedule_kind, "
                "schedule_json = excluded.schedule_json, "
                "action_key = excluded.action_key, "
                "action_payload_json = excluded.action_payload_json, "
                "execution_policy_json = excluded.execution_policy_json, "
                "result_policy_json = excluded.result_policy_json, "
                "metadata_json = excluded.metadata_json, "
                "version = excluded.version, "
                "updated_at = excluded.updated_at",
                definition.id.value,
                definition.key,
                schedule_kind_name(definition.schedule),
                schedule_to_json(definition.schedule).dump(),
                definition.action.action_key,
                definition.action.payload.dump(),
                execution_policy_to_json(definition.execution).dump(),
                result_policy_to_json(definition.result).dump(),
                definition.metadata.dump(),
                definition.version,
                now,
                now);
            if (!save_definition) {
                return save_definition;
            }

            return run_bound(
                tx,
                "INSERT INTO automation_job_state ("
                "job_id, enabled, paused, next_due_at, last_scheduled_at, last_started_at, last_finished_at, last_status, in_flight_count, lease_owner, lease_expires_at, revision, updated_at"
                ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13) "
                "ON CONFLICT(job_id) DO UPDATE SET "
                "enabled = excluded.enabled, "
                "paused = excluded.paused, "
                "next_due_at = excluded.next_due_at, "
                "last_scheduled_at = excluded.last_scheduled_at, "
                "last_started_at = excluded.last_started_at, "
                "last_finished_at = excluded.last_finished_at, "
                "last_status = excluded.last_status, "
                "in_flight_count = excluded.in_flight_count, "
                "lease_owner = excluded.lease_owner, "
                "lease_expires_at = excluded.lease_expires_at, "
                "revision = excluded.revision, "
                "updated_at = excluded.updated_at",
                definition.id.value,
                state.enabled ? 1 : 0,
                state.paused ? 1 : 0,
                state.next_due_at,
                state.last_scheduled_at,
                state.last_started_at,
                state.last_finished_at,
                state.last_status,
                state.in_flight_count,
                state.lease_owner,
                state.lease_expires_at,
                state.revision,
                now);
        });
    }

    auto SqliteJobStore::load_job(const JobId &job_id) const -> StoreResult<std::optional<StoredJob>> {
        validate_non_blank(job_id.value, "job id");

        std::scoped_lock lock(mutex_);
        auto query = db_.query(
            "SELECT d.job_id, d.job_key, d.schedule_kind, d.schedule_json, d.action_key, d.action_payload_json, d.execution_policy_json, d.result_policy_json, d.metadata_json, d.version, "
            "s.enabled, s.paused, s.next_due_at, s.last_scheduled_at, s.last_started_at, s.last_finished_at, s.last_status, s.in_flight_count, s.lease_owner, s.lease_expires_at, s.revision "
            "FROM automation_job_definitions d "
            "JOIN automation_job_state s ON s.job_id = d.job_id "
            "WHERE d.job_id = ?1 "
            "LIMIT 1");
        if (!query) {
            return std::unexpected(query.error());
        }
        auto row = query->bind(job_id.value).template optional<load_job_row>();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (!row->has_value()) {
            return std::optional<StoredJob>{};
        }

        auto decoded = decode_stored_job(**row);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return std::optional<StoredJob>(std::move(*decoded));
    }

    auto SqliteJobStore::remove_job(const JobId &job_id) -> StoreResult<bool> {
        validate_non_blank(job_id.value, "job id");

        std::scoped_lock lock(mutex_);
        auto removed = run_bound(db_, "DELETE FROM automation_job_definitions WHERE job_id = ?1", job_id.value);
        if (!removed) {
            return std::unexpected(removed.error());
        }
        return db_.changes() > 0;
    }

    auto SqliteJobStore::next_due_at() const -> StoreResult<std::optional<std::int64_t>> {
        std::scoped_lock lock(mutex_);
        auto query = db_.query(
            "SELECT MIN(next_due_at) "
            "FROM automation_job_state "
            "WHERE enabled = 1 "
            "AND paused = 0 "
            "AND next_due_at IS NOT NULL");
        if (!query) {
            return std::unexpected(query.error());
        }
        auto next_due = query->template optional<std::int64_t>();
        if (!next_due) {
            return std::unexpected(next_due.error());
        }
        return *next_due;
    }

    auto SqliteJobStore::list_due(std::int64_t now, std::size_t limit) const -> StoreResult<std::vector<JobId>> {
        if (limit == 0) {
            return std::vector<JobId>{};
        }

        std::scoped_lock lock(mutex_);
        auto query = db_.query(
            "SELECT job_id "
            "FROM automation_job_state "
            "WHERE enabled = 1 "
            "AND paused = 0 "
            "AND next_due_at IS NOT NULL "
            "AND next_due_at <= ?1 "
            "ORDER BY next_due_at ASC, job_id ASC "
            "LIMIT ?2");
        if (!query) {
            return std::unexpected(query.error());
        }
        auto rows = query->bind(now, static_cast<std::int64_t>(limit)).template all<std::string>();
        if (!rows) {
            return std::unexpected(rows.error());
        }

        std::vector<JobId> due_jobs;
        due_jobs.reserve(rows->size());
        for (auto &job_id : *rows) {
            due_jobs.push_back(JobId{.value = std::move(job_id)});
        }
        return due_jobs;
    }

    auto SqliteJobStore::ensure_schema() -> StoreResult<void> {
        auto foreign_keys = db_.exec_script("PRAGMA foreign_keys = ON;", "enable automation foreign keys");
        if (!foreign_keys) {
            return std::unexpected(foreign_keys.error());
        }

        auto schema = db_.exec_script(
            "CREATE TABLE IF NOT EXISTS automation_job_definitions ("
            "    job_id TEXT PRIMARY KEY,"
            "    job_key TEXT NOT NULL UNIQUE,"
            "    schedule_kind TEXT NOT NULL,"
            "    schedule_json TEXT NOT NULL,"
            "    action_key TEXT NOT NULL,"
            "    action_payload_json TEXT NOT NULL,"
            "    execution_policy_json TEXT NOT NULL,"
            "    result_policy_json TEXT NOT NULL,"
            "    metadata_json TEXT NOT NULL,"
            "    version INTEGER NOT NULL DEFAULT 0,"
            "    created_at INTEGER NOT NULL,"
            "    updated_at INTEGER NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS automation_job_state ("
            "    job_id TEXT PRIMARY KEY,"
            "    enabled INTEGER NOT NULL DEFAULT 1,"
            "    paused INTEGER NOT NULL DEFAULT 0,"
            "    next_due_at INTEGER,"
            "    last_scheduled_at INTEGER,"
            "    last_started_at INTEGER,"
            "    last_finished_at INTEGER,"
            "    last_status TEXT NOT NULL DEFAULT '',"
            "    in_flight_count INTEGER NOT NULL DEFAULT 0,"
            "    lease_owner TEXT NOT NULL DEFAULT '',"
            "    lease_expires_at INTEGER,"
            "    revision INTEGER NOT NULL DEFAULT 0,"
            "    updated_at INTEGER NOT NULL,"
            "    FOREIGN KEY(job_id) REFERENCES automation_job_definitions(job_id) ON DELETE CASCADE"
            ");"
            "CREATE TABLE IF NOT EXISTS automation_executions ("
            "    execution_id TEXT PRIMARY KEY,"
            "    job_id TEXT NOT NULL,"
            "    scheduled_for INTEGER NOT NULL,"
            "    dispatch_reason TEXT NOT NULL DEFAULT 'scheduled',"
            "    attempt INTEGER NOT NULL DEFAULT 0,"
            "    started_at INTEGER NOT NULL,"
            "    finished_at INTEGER,"
            "    status TEXT NOT NULL DEFAULT '',"
            "    summary TEXT NOT NULL DEFAULT '',"
            "    reply_ref TEXT NOT NULL DEFAULT '',"
            "    delivery_status TEXT NOT NULL DEFAULT '',"
            "    driver_id TEXT NOT NULL DEFAULT '',"
            "    FOREIGN KEY(job_id) REFERENCES automation_job_definitions(job_id) ON DELETE CASCADE"
            ");"
            "CREATE TABLE IF NOT EXISTS automation_deliveries ("
            "    id TEXT PRIMARY KEY,"
            "    execution_id TEXT NOT NULL,"
            "    target TEXT NOT NULL,"
            "    status TEXT NOT NULL DEFAULT '',"
            "    title TEXT NOT NULL DEFAULT '',"
            "    body TEXT NOT NULL DEFAULT '',"
            "    created_at INTEGER NOT NULL,"
            "    acked_at INTEGER"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_automation_job_state_due ON automation_job_state(enabled, paused, next_due_at);"
            "CREATE INDEX IF NOT EXISTS idx_automation_job_state_lease ON automation_job_state(lease_expires_at);"
            "CREATE INDEX IF NOT EXISTS idx_automation_executions_job_started ON automation_executions(job_id, started_at DESC);",
            "create automation core store schema");
        if (!schema) {
            return std::unexpected(schema.error());
        }

        return {};
    }

} // namespace orangutan::automation
