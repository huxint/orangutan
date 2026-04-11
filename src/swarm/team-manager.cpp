#include "swarm/team-manager.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace orangutan::swarm {

    namespace {

        std::string generate_team_id() {
            static std::atomic<std::uint64_t> counter{0};
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            auto seq = counter.fetch_add(1, std::memory_order_relaxed);
            return "team-" + std::to_string(us) + "-" + std::to_string(seq);
        }

        std::int64_t now_millis() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

    } // namespace

    struct TeamManager::Impl {
        sqlite3 *db = nullptr;
        mutable std::mutex mutex;

        explicit Impl(const std::filesystem::path &db_path) {
            const auto db_path_text = db_path.string();
            int rc = sqlite3_open(db_path_text.c_str(), &db);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(db);
                sqlite3_close(db);
                db = nullptr;
                throw std::runtime_error("Failed to open team manager database: " + err);
            }

            sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

            const char *create_teams_sql = "CREATE TABLE IF NOT EXISTS teams ("
                                           "    id TEXT PRIMARY KEY,"
                                           "    name TEXT NOT NULL UNIQUE,"
                                           "    description TEXT NOT NULL DEFAULT '',"
                                           "    lead_agent_id TEXT NOT NULL,"
                                           "    created_at INTEGER NOT NULL,"
                                           "    active INTEGER NOT NULL DEFAULT 1"
                                           ");";

            const char *create_members_sql = "CREATE TABLE IF NOT EXISTS team_members ("
                                             "    agent_id TEXT NOT NULL,"
                                             "    name TEXT NOT NULL,"
                                             "    agent_key TEXT NOT NULL,"
                                             "    team_id TEXT NOT NULL REFERENCES teams(id),"
                                             "    joined_at INTEGER NOT NULL,"
                                             "    active INTEGER NOT NULL DEFAULT 1,"
                                             "    PRIMARY KEY (team_id, agent_id)"
                                             ");";

            char *err_msg = nullptr;
            rc = sqlite3_exec(db, create_teams_sql, nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                std::string err = err_msg != nullptr ? err_msg : "unknown error";
                sqlite3_free(err_msg);
                throw std::runtime_error("Failed to create teams table: " + err);
            }

            rc = sqlite3_exec(db, create_members_sql, nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                std::string err = err_msg != nullptr ? err_msg : "unknown error";
                sqlite3_free(err_msg);
                throw std::runtime_error("Failed to create team_members table: " + err);
            }
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;

        ~Impl() {
            if (db != nullptr) {
                sqlite3_close(db);
            }
        }
    };

    TeamManager::TeamManager(const std::filesystem::path &db_path)
    : impl_(std::make_unique<Impl>(db_path)) {}

    TeamManager::~TeamManager() = default;

    TeamRecord TeamManager::create_team(const std::string &name, const std::string &description, const std::string &lead_agent_id) {
        std::scoped_lock lock(impl_->mutex);

        TeamRecord record;
        record.id = generate_team_id();
        record.name = name;
        record.description = description;
        record.lead_agent_id = lead_agent_id;
        record.created_at = now_millis();
        record.active = true;

        const char *sql = "INSERT INTO teams (id, name, description, lead_agent_id, created_at, active) VALUES (?, ?, ?, ?, ?, 1);";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("Failed to prepare team insert: {}", sqlite3_errmsg(impl_->db));
            return record;
        }

        sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, lead_agent_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, record.created_at);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            spdlog::error("Failed to insert team: {}", sqlite3_errmsg(impl_->db));
        }
        sqlite3_finalize(stmt);

        return record;
    }

    std::optional<TeamRecord> TeamManager::find_team(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE id = ?;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return std::nullopt;
        }

        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<TeamRecord> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            TeamRecord record;
            record.id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            record.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            record.description = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            record.lead_agent_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            record.created_at = sqlite3_column_int64(stmt, 4);
            record.active = sqlite3_column_int(stmt, 5) != 0;
            result = std::move(record);
        }

        sqlite3_finalize(stmt);
        return result;
    }

    std::optional<TeamRecord> TeamManager::find_team_by_name(const std::string &name) const {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE name = ?;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return std::nullopt;
        }

        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<TeamRecord> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            TeamRecord record;
            record.id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            record.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            record.description = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            record.lead_agent_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            record.created_at = sqlite3_column_int64(stmt, 4);
            record.active = sqlite3_column_int(stmt, 5) != 0;
            result = std::move(record);
        }

        sqlite3_finalize(stmt);
        return result;
    }

    void TeamManager::delete_team(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        // Delete members first
        const char *del_members_sql = "DELETE FROM team_members WHERE team_id = ?;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, del_members_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Delete team
        const char *del_team_sql = "DELETE FROM teams WHERE id = ?;";
        rc = sqlite3_prepare_v2(impl_->db, del_team_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void TeamManager::add_member(const TeamMemberRecord &member) {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "INSERT INTO team_members (agent_id, name, agent_key, team_id, joined_at, active) VALUES (?, ?, ?, ?, ?, 1);";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("Failed to prepare member insert: {}", sqlite3_errmsg(impl_->db));
            return;
        }

        auto joined = member.joined_at != 0 ? member.joined_at : now_millis();

        sqlite3_bind_text(stmt, 1, member.agent_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, member.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, member.agent_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, member.team_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, joined);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            spdlog::error("Failed to insert team member: {}", sqlite3_errmsg(impl_->db));
        }
        sqlite3_finalize(stmt);
    }

    std::vector<TeamMemberRecord> TeamManager::list_members(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "SELECT agent_id, name, agent_key, team_id, joined_at, active FROM team_members WHERE team_id = ? AND active = 1;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return {};
        }

        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);

        std::vector<TeamMemberRecord> result;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TeamMemberRecord member;
            member.agent_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            member.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            member.agent_key = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            member.team_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            member.joined_at = sqlite3_column_int64(stmt, 4);
            member.active = sqlite3_column_int(stmt, 5) != 0;
            result.push_back(std::move(member));
        }

        sqlite3_finalize(stmt);
        return result;
    }

    std::vector<std::string> TeamManager::list_member_names(const std::string &team_id) const {
        auto members = list_members(team_id);
        std::vector<std::string> names;
        names.reserve(members.size());
        for (auto &m : members) {
            names.push_back(std::move(m.name));
        }
        return names;
    }

    void TeamManager::deactivate_member(const std::string &team_id, const std::string &agent_id) {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "UPDATE team_members SET active = 0 WHERE team_id = ? AND agent_id = ?;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return;
        }

        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void TeamManager::abandon_active_members(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "UPDATE team_members SET active = 0 WHERE team_id = ? AND active = 1;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return;
        }

        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    std::vector<TeamRecord> TeamManager::list_active_teams() const {
        std::scoped_lock lock(impl_->mutex);

        const char *sql = "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE active = 1;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return {};
        }

        std::vector<TeamRecord> result;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TeamRecord record;
            record.id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            record.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            record.description = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            record.lead_agent_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            record.created_at = sqlite3_column_int64(stmt, 4);
            record.active = sqlite3_column_int(stmt, 5) != 0;
            result.push_back(std::move(record));
        }

        sqlite3_finalize(stmt);
        return result;
    }

} // namespace orangutan::swarm
