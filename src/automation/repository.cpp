#include "automation/repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include <nlohmann/json.hpp>

#include "automation/parser.hpp"
#include "storage/sqlite-throwing.hpp"
#include "utils/string.hpp"

namespace orangutan::automation {
    namespace {

        using automation_row =
            std::tuple<std::string, std::string, std::string, int, int, std::string, std::string, std::string, std::string, std::string, std::optional<base::i64>,
                       std::optional<base::i64>, std::string>;
        using run_row =
            std::tuple<std::string, std::string, std::string, std::string, base::i64, std::optional<base::i64>, std::string, std::string, std::string, std::string, std::string>;
        using delivery_row =
            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string, std::string, base::i64, std::optional<base::i64>>;

        [[nodiscard]]
        std::filesystem::path default_db_path() {
            const char *home = std::getenv("HOME");
            if (home == nullptr || std::string_view(home).empty()) {
                return std::filesystem::path{"automation.db"};
            }

            return std::filesystem::path(home) / ".orangutan" / "automation.db";
        }

        [[nodiscard]]
        bool is_blank(std::string_view value) {
            return utils::trim_copy(value).empty();
        }

        void validate_non_blank(std::string_view value, std::string_view label) {
            if (is_blank(value)) {
                throw std::invalid_argument(std::string(label) + " must not be blank");
            }
        }

        void validate_automation_definition(const Automation &automation) {
            validate_non_blank(automation.agent_key, "agent key");
            validate_non_blank(automation.name, "automation name");
            validate_non_blank(automation.prompt, "prompt");

            if (automation.enabled == false && automation.paused == true) {
                throw std::invalid_argument("disabled automations must not be paused");
            }

            if (automation.delivery.mode == delivery_mode::notify && automation.delivery.targets.empty()) {
                throw std::invalid_argument("notify delivery requires at least one target");
            }

            for (const auto &target : automation.delivery.targets) {
                validate_non_blank(target, "delivery target");
            }

            for (const auto &tag : automation.tags) {
                validate_non_blank(tag, "tag");
            }

            const auto normalized_trigger = trigger_from_json(trigger_to_json(automation.trigger));
            if (!normalized_trigger.has_value()) {
                throw std::invalid_argument(normalized_trigger.error());
            }
        }

        [[nodiscard]]
        std::optional<int> to_optional_sqlite_bool(const std::optional<bool> &value) {
            if (!value.has_value()) {
                return std::nullopt;
            }
            return *value ? 1 : 0;
        }

        [[nodiscard]]
        std::vector<std::string> parse_tags(std::string_view value) {
            std::vector<std::string> tags;
            const auto parsed = nlohmann::json::parse(value, nullptr, false);
            if (parsed.is_discarded()) {
                throw std::runtime_error("stored tags_json is invalid");
            }
            if (!parsed.is_array()) {
                throw std::runtime_error("stored tags_json must be an array");
            }

            for (const auto &entry : parsed) {
                if (!entry.is_string()) {
                    throw std::runtime_error("stored tags_json entries must be strings");
                }
                tags.push_back(entry.get<std::string>());
            }

            return tags;
        }

        [[nodiscard]]
        Automation read_automation(const automation_row &row) {
            const auto trigger_value = nlohmann::json::parse(std::get<8>(row));
            const auto parsed_trigger = trigger_from_json(trigger_value);
            if (!parsed_trigger.has_value()) {
                throw std::runtime_error("stored trigger_json is invalid: " + parsed_trigger.error());
            }

            return Automation{
                .id = std::get<0>(row),
                .agent_key = std::get<1>(row),
                .name = std::get<2>(row),
                .prompt = std::get<5>(row),
                .notes = std::get<6>(row),
                .trigger = *parsed_trigger,
                .delivery = delivery_policy_from_json(nlohmann::json::parse(std::get<9>(row))),
                .tags = parse_tags(std::get<7>(row)),
                .last_run_at = std::get<10>(row),
                .next_due_at = std::get<11>(row),
                .last_status = std::get<12>(row),
                .enabled = std::get<3>(row) != 0,
                .paused = std::get<4>(row) != 0,
            };
        }

        [[nodiscard]]
        RunRecord read_run(const run_row &row) {
            return RunRecord{
                .id = std::get<0>(row),
                .automation_id = std::get<1>(row),
                .agent_key = std::get<2>(row),
                .automation_name = std::get<3>(row),
                .started_at = std::get<4>(row),
                .finished_at = std::get<5>(row),
                .status = std::get<6>(row),
                .summary = std::get<7>(row),
                .reply = std::get<8>(row),
                .delivery_status = std::get<9>(row),
                .log_path = std::get<10>(row),
            };
        }

        [[nodiscard]]
        DeliveryRecord read_delivery(const delivery_row &row) {
            return DeliveryRecord{
                .id = std::get<0>(row),
                .run_id = std::get<1>(row),
                .automation_id = std::get<2>(row),
                .agent_key = std::get<3>(row),
                .target = std::get<4>(row),
                .status = std::get<5>(row),
                .title = std::get<6>(row),
                .body = std::get<7>(row),
                .created_at = std::get<8>(row),
                .acked_at = std::get<9>(row),
            };
        }

        [[nodiscard]]
        std::optional<automation_row> find_automation_row_by_id(const sqlite::Database &db, std::string_view agent_key, std::string_view id) {
            return sqlite::query_optional<automation_row>(
                db,
                "SELECT id, agent_key, name, enabled, paused, prompt, notes, tags_json, trigger_json, delivery_json, last_run_at, next_due_at, last_status "
                "FROM automations "
                "WHERE agent_key = ?1 AND id = ?2 "
                "LIMIT 1",
                agent_key, id);
        }

        [[nodiscard]]
        std::optional<automation_row> find_automation_row_by_name(const sqlite::Database &db, std::string_view agent_key, std::string_view name) {
            return sqlite::query_optional<automation_row>(
                db,
                "SELECT id, agent_key, name, enabled, paused, prompt, notes, tags_json, trigger_json, delivery_json, last_run_at, next_due_at, last_status "
                "FROM automations "
                "WHERE agent_key = ?1 AND name = ?2 "
                "LIMIT 1",
                agent_key, name);
        }

    } // namespace

    Repository::Repository()
    : Repository(default_db_path()) {}

    Repository::Repository(const std::filesystem::path &db_path)
    : db_(sqlite::open_or_throw(db_path)) {
        ensure_schema();
    }

    std::string Repository::save(const Automation &automation_input) {
        std::scoped_lock lock(mutex_);
        auto automation = automation_input;
        validate_automation_definition(automation);

        if (automation.id.empty()) {
            automation.id = generate_id("auto");
        }

        const auto now = to_unix_seconds(Clock::now());
        sqlite::exec_bind(db_,
                          "INSERT INTO automations ("
                          "id, agent_key, name, enabled, paused, prompt, notes, tags_json, trigger_json, delivery_json, last_run_at, next_due_at, last_status, created_at, updated_at"
                          ") VALUES ("
                          "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15"
                          ") "
                          "ON CONFLICT(id) DO UPDATE SET "
                          "agent_key = excluded.agent_key, "
                          "name = excluded.name, "
                          "enabled = excluded.enabled, "
                          "paused = excluded.paused, "
                          "prompt = excluded.prompt, "
                          "notes = excluded.notes, "
                          "tags_json = excluded.tags_json, "
                          "trigger_json = excluded.trigger_json, "
                          "delivery_json = excluded.delivery_json, "
                          "last_run_at = excluded.last_run_at, "
                          "next_due_at = excluded.next_due_at, "
                          "last_status = excluded.last_status, "
                          "updated_at = excluded.updated_at",
                          automation.id,
                          automation.agent_key,
                          automation.name,
                          automation.enabled ? 1 : 0,
                          automation.paused ? 1 : 0,
                          automation.prompt,
                          automation.notes,
                          nlohmann::json(automation.tags).dump(),
                          trigger_to_json(automation.trigger).dump(),
                          delivery_policy_to_json(automation.delivery).dump(),
                          automation.last_run_at,
                          automation.next_due_at,
                          automation.last_status,
                          now,
                          now);

        return automation.id;
    }

    std::optional<Automation> Repository::find(std::string_view agent_key, std::string_view id_or_name) const {
        std::scoped_lock lock(mutex_);
        if (const auto row = find_automation_row_by_id(db_, agent_key, id_or_name); row.has_value()) {
            return read_automation(*row);
        }

        if (const auto row = find_automation_row_by_name(db_, agent_key, id_or_name); row.has_value()) {
            return read_automation(*row);
        }

        return std::nullopt;
    }

    std::vector<Automation> Repository::list(const AutomationQuery &query) const {
        std::scoped_lock lock(mutex_);
        std::vector<Automation> automations;
        const auto enabled_filter = to_optional_sqlite_bool(query.enabled);
        const auto paused_filter = to_optional_sqlite_bool(query.paused);

        const auto rows = sqlite::query_all<automation_row>(
            db_,
            "SELECT id, agent_key, name, enabled, paused, prompt, notes, tags_json, trigger_json, delivery_json, last_run_at, next_due_at, "
            "last_status "
            "FROM automations "
            "WHERE (?1 = '' OR agent_key = ?1) "
            "AND (?2 IS NULL OR enabled = ?2) "
            "AND (?3 IS NULL OR paused = ?3) "
            "ORDER BY agent_key, name",
            query.agent_key, enabled_filter, paused_filter);
        for (const auto &row : rows) {
            automations.push_back(read_automation(row));
        }

        return automations;
    }

    bool Repository::remove(std::string_view agent_key, std::string_view id_or_name) {
        std::scoped_lock lock(mutex_);

        const auto row = [&]() -> std::optional<automation_row> {
            if (const auto by_id = find_automation_row_by_id(db_, agent_key, id_or_name); by_id.has_value()) {
                return by_id;
            }
            return find_automation_row_by_name(db_, agent_key, id_or_name);
        }();

        if (!row.has_value()) {
            return false;
        }

        sqlite::exec_bind(db_, "DELETE FROM automations WHERE id = ?1", std::get<0>(*row));
        return db_.changes() > 0;
    }

    std::string Repository::insert_run(const RunRecord &run_input) {
        std::scoped_lock lock(mutex_);
        auto run = run_input;

        validate_non_blank(run.automation_id, "automation id");
        validate_non_blank(run.agent_key, "agent key");

        if (run.id.empty()) {
            run.id = generate_id("run");
        }

        sqlite::exec_bind(db_,
                          "INSERT INTO automation_runs ("
                          "id, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, reply, delivery_status, log_path"
                          ") VALUES ("
                          "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11"
                          ")",
                          run.id,
                          run.automation_id,
                          run.agent_key,
                          run.automation_name,
                          run.started_at,
                          run.finished_at,
                          run.status,
                          run.summary,
                          run.reply,
                          run.delivery_status,
                          run.log_path);

        return run.id;
    }

    std::vector<RunRecord> Repository::list_runs(const RunQuery &query) const {
        std::scoped_lock lock(mutex_);
        std::vector<RunRecord> runs;

        const auto rows = sqlite::query_all<run_row>(
            db_,
            "SELECT id, automation_id, agent_key, automation_name, started_at, finished_at, status, summary, reply, delivery_status, log_path "
            "FROM automation_runs "
            "WHERE (?1 = '' OR agent_key = ?1) "
            "AND (?2 = '' OR automation_id = ?2) "
            "ORDER BY started_at DESC",
            query.agent_key, query.automation_id);
        for (const auto &row : rows) {
            runs.push_back(read_run(row));
        }

        return runs;
    }

    std::string Repository::insert_delivery(const DeliveryRecord &delivery_input) {
        std::scoped_lock lock(mutex_);
        auto delivery = delivery_input;

        validate_non_blank(delivery.run_id, "run id");
        validate_non_blank(delivery.automation_id, "automation id");
        validate_non_blank(delivery.agent_key, "agent key");
        validate_non_blank(delivery.target, "delivery target");

        if (delivery.id.empty()) {
            delivery.id = generate_id("delivery");
        }

        sqlite::exec_bind(db_,
                          "INSERT INTO automation_deliveries ("
                          "id, run_id, automation_id, agent_key, target, status, title, body, created_at, acked_at"
                          ") VALUES ("
                          "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10"
                          ")",
                          delivery.id,
                          delivery.run_id,
                          delivery.automation_id,
                          delivery.agent_key,
                          delivery.target,
                          delivery.status,
                          delivery.title,
                          delivery.body,
                          delivery.created_at,
                          delivery.acked_at);

        return delivery.id;
    }

    std::vector<DeliveryRecord> Repository::list_deliveries(const DeliveryQuery &query) const {
        std::scoped_lock lock(mutex_);
        std::vector<DeliveryRecord> deliveries;

        const auto rows = sqlite::query_all<delivery_row>(
            db_,
            "SELECT id, run_id, automation_id, agent_key, target, status, title, body, created_at, acked_at "
            "FROM automation_deliveries "
            "WHERE (?1 = '' OR agent_key = ?1) "
            "AND (?2 = '' OR automation_id = ?2) "
            "AND (?3 = '' OR run_id = ?3) "
            "AND (?4 = '' OR target = ?4) "
            "AND (?5 = 0 OR acked_at IS NULL) "
            "ORDER BY created_at DESC",
            query.agent_key, query.automation_id, query.run_id, query.target, query.only_unacked ? 1 : 0);
        for (const auto &row : rows) {
            deliveries.push_back(read_delivery(row));
        }

        return deliveries;
    }

    std::optional<DeliveryRecord> Repository::ack_delivery(std::string_view agent_key, std::string_view delivery_id, std::optional<base::i64> acked_at) {
        std::scoped_lock lock(mutex_);
        validate_non_blank(agent_key, "agent key");
        validate_non_blank(delivery_id, "delivery id");

        const auto resolved_acked_at = acked_at.value_or(to_unix_seconds(Clock::now()));
        sqlite::exec_bind(db_,
                          "UPDATE automation_deliveries "
                          "SET acked_at = ?3 "
                          "WHERE agent_key = ?1 AND id = ?2 AND acked_at IS NULL",
                          agent_key, delivery_id, resolved_acked_at);

        if (db_.changes() == 0) {
            return std::nullopt;
        }

        const auto row = sqlite::query_optional<delivery_row>(
            db_,
            "SELECT id, run_id, automation_id, agent_key, target, status, title, body, created_at, acked_at "
            "FROM automation_deliveries "
            "WHERE agent_key = ?1 AND id = ?2 "
            "LIMIT 1",
            agent_key, delivery_id);
        if (!row.has_value()) {
            return std::nullopt;
        }

        return read_delivery(*row);
    }

    void Repository::clear_deliveries(const DeliveryQuery &query, std::optional<base::i64> acked_at) {
        std::scoped_lock lock(mutex_);
        if (query.agent_key.empty()) {
            throw std::invalid_argument("clear_deliveries requires agent_key");
        }

        const auto resolved_acked_at = acked_at.value_or(to_unix_seconds(Clock::now()));
        sqlite::exec_bind(db_,
                          "UPDATE automation_deliveries "
                          "SET acked_at = ?1 "
                          "WHERE agent_key = ?2 "
                          "AND (?3 = '' OR automation_id = ?3) "
                          "AND (?4 = '' OR run_id = ?4) "
                          "AND (?5 = '' OR target = ?5) "
                          "AND acked_at IS NULL",
                          resolved_acked_at, query.agent_key, query.automation_id, query.run_id, query.target);
    }

    void Repository::ensure_schema() {
        sqlite::exec_script(db_,
                            "CREATE TABLE IF NOT EXISTS automations ("
                            "  id TEXT PRIMARY KEY,"
                            "  agent_key TEXT NOT NULL,"
                            "  name TEXT NOT NULL,"
                            "  enabled INTEGER NOT NULL,"
                            "  paused INTEGER NOT NULL DEFAULT 0,"
                            "  prompt TEXT NOT NULL,"
                            "  notes TEXT NOT NULL DEFAULT '',"
                            "  tags_json TEXT NOT NULL,"
                            "  trigger_json TEXT NOT NULL,"
                            "  delivery_json TEXT NOT NULL,"
                            "  last_run_at INTEGER,"
                            "  next_due_at INTEGER,"
                            "  last_status TEXT NOT NULL DEFAULT '',"
                            "  created_at INTEGER NOT NULL,"
                            "  updated_at INTEGER NOT NULL"
                            ");",
                            "create automations table");
        sqlite::exec_script(db_, "CREATE UNIQUE INDEX IF NOT EXISTS idx_automations_agent_name ON automations(agent_key, name);", "create automations unique name index");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_automations_scheduler ON automations(agent_key, enabled, paused, next_due_at);", "create automations scheduler index");

        sqlite::exec_script(db_,
                            "CREATE TABLE IF NOT EXISTS automation_runs ("
                            "  id TEXT PRIMARY KEY,"
                            "  automation_id TEXT NOT NULL,"
                            "  agent_key TEXT NOT NULL,"
                            "  automation_name TEXT NOT NULL DEFAULT '',"
                            "  started_at INTEGER NOT NULL,"
                            "  finished_at INTEGER,"
                            "  status TEXT NOT NULL DEFAULT '',"
                            "  summary TEXT NOT NULL DEFAULT '',"
                            "  reply TEXT NOT NULL DEFAULT '',"
                            "  delivery_status TEXT NOT NULL DEFAULT '',"
                            "  log_path TEXT NOT NULL DEFAULT ''"
                            ");",
                            "create automation_runs table");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_automation_runs_lookup ON automation_runs(agent_key, automation_id, started_at);", "create automation_runs index");

        sqlite::exec_script(db_,
                            "CREATE TABLE IF NOT EXISTS automation_deliveries ("
                            "  id TEXT PRIMARY KEY,"
                            "  run_id TEXT NOT NULL,"
                            "  automation_id TEXT NOT NULL,"
                            "  agent_key TEXT NOT NULL,"
                            "  target TEXT NOT NULL,"
                            "  status TEXT NOT NULL DEFAULT '',"
                            "  title TEXT NOT NULL DEFAULT '',"
                            "  body TEXT NOT NULL DEFAULT '',"
                            "  created_at INTEGER NOT NULL,"
                            "  acked_at INTEGER"
                            ");",
                            "create automation_deliveries table");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_automation_deliveries_lookup ON automation_deliveries(agent_key, automation_id, run_id, created_at);",
                            "create automation_deliveries lookup index");
        sqlite::exec_script(db_, "CREATE INDEX IF NOT EXISTS idx_automation_deliveries_ack ON automation_deliveries(agent_key, acked_at, created_at);",
                            "create automation_deliveries ack index");
    }

} // namespace orangutan::automation
