#include "orchestration/team-manager.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "storage/sqlite-throwing.hpp"

namespace orangutan::sqlite {

    template <>
    struct RowMapper<orangutan::orchestration::TeamRecord> {
        static auto map(const Row &row) -> SqliteResult<orangutan::orchestration::TeamRecord> {
            auto id = row.get<std::string>(0);
            if (!id) {
                return std::unexpected(id.error());
            }
            auto name = row.get<std::string>(1);
            if (!name) {
                return std::unexpected(name.error());
            }
            auto description = row.get<std::string>(2);
            if (!description) {
                return std::unexpected(description.error());
            }
            auto lead_agent_id = row.get<std::string>(3);
            if (!lead_agent_id) {
                return std::unexpected(lead_agent_id.error());
            }
            auto created_at = row.get<std::int64_t>(4);
            if (!created_at) {
                return std::unexpected(created_at.error());
            }
            auto active = row.get<int>(5);
            if (!active) {
                return std::unexpected(active.error());
            }
            return orangutan::orchestration::TeamRecord{
                .id = std::move(*id),
                .name = std::move(*name),
                .description = std::move(*description),
                .lead_agent_id = std::move(*lead_agent_id),
                .created_at = *created_at,
                .active = *active != 0,
            };
        }
    };

    template <>
    struct RowMapper<orangutan::orchestration::TeamMemberRecord> {
        static auto map(const Row &row) -> SqliteResult<orangutan::orchestration::TeamMemberRecord> {
            auto agent_id = row.get<std::string>(0);
            if (!agent_id) {
                return std::unexpected(agent_id.error());
            }
            auto name = row.get<std::string>(1);
            if (!name) {
                return std::unexpected(name.error());
            }
            auto agent_key = row.get<std::string>(2);
            if (!agent_key) {
                return std::unexpected(agent_key.error());
            }
            auto team_id = row.get<std::string>(3);
            if (!team_id) {
                return std::unexpected(team_id.error());
            }
            auto joined_at = row.get<std::int64_t>(4);
            if (!joined_at) {
                return std::unexpected(joined_at.error());
            }
            auto active = row.get<int>(5);
            if (!active) {
                return std::unexpected(active.error());
            }
            return orangutan::orchestration::TeamMemberRecord{
                .agent_id = std::move(*agent_id),
                .name = std::move(*name),
                .agent_key = std::move(*agent_key),
                .team_id = std::move(*team_id),
                .joined_at = *joined_at,
                .active = *active != 0,
            };
        }
    };

} // namespace orangutan::sqlite

namespace orangutan::orchestration {

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
        sqlite::Database db;
        mutable std::mutex mutex;

        explicit Impl(const std::filesystem::path &db_path)
        : db(sqlite::open_or_throw(db_path)) {
            sqlite::exec_script(db, "PRAGMA journal_mode=WAL;", "failed to enable team manager WAL mode");
            sqlite::exec_script(db,
                                "CREATE TABLE IF NOT EXISTS teams ("
                                "    id TEXT PRIMARY KEY,"
                                "    name TEXT NOT NULL UNIQUE,"
                                "    description TEXT NOT NULL DEFAULT '',"
                                "    lead_agent_id TEXT NOT NULL,"
                                "    created_at INTEGER NOT NULL,"
                                "    active INTEGER NOT NULL DEFAULT 1"
                                ");"
                                "CREATE TABLE IF NOT EXISTS team_members ("
                                "    agent_id TEXT NOT NULL,"
                                "    name TEXT NOT NULL,"
                                "    agent_key TEXT NOT NULL,"
                                "    team_id TEXT NOT NULL REFERENCES teams(id),"
                                "    joined_at INTEGER NOT NULL,"
                                "    active INTEGER NOT NULL DEFAULT 1,"
                                "    PRIMARY KEY (team_id, agent_id)"
                                ");",
                                "failed to create team manager tables");
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;

        ~Impl() = default;
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

        try {
            sqlite::exec_bind(impl_->db,
                              "INSERT INTO teams (id, name, description, lead_agent_id, created_at, active) VALUES (?, ?, ?, ?, ?, 1)",
                              record.id, name, description, lead_agent_id, record.created_at);
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert team: {}", ex.what());
        }

        return record;
    }

    std::optional<TeamRecord> TeamManager::find_team(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return sqlite::query_optional<TeamRecord>(
                impl_->db,
                "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE id = ?",
                team_id);
        } catch (const std::exception &ex) {
            spdlog::error("failed to find team {}: {}", team_id, ex.what());
            return std::nullopt;
        }
    }

    std::optional<TeamRecord> TeamManager::find_team_by_name(const std::string &name) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return sqlite::query_optional<TeamRecord>(
                impl_->db,
                "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE name = ?",
                name);
        } catch (const std::exception &ex) {
            spdlog::error("failed to find team by name {}: {}", name, ex.what());
            return std::nullopt;
        }
    }

    void TeamManager::delete_team(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        try {
            sqlite::unwrap(impl_->db.transaction([&](sqlite::Database &tx) {
                sqlite::exec_bind(tx, "DELETE FROM team_members WHERE team_id = ?", team_id);
                sqlite::exec_bind(tx, "DELETE FROM teams WHERE id = ?", team_id);
            }));
        } catch (const std::exception &ex) {
            spdlog::error("failed to delete team {}: {}", team_id, ex.what());
        }
    }

    void TeamManager::add_member(const TeamMemberRecord &member) {
        std::scoped_lock lock(impl_->mutex);

        try {
            sqlite::exec_bind(impl_->db,
                              "INSERT INTO team_members (agent_id, name, agent_key, team_id, joined_at, active) VALUES (?, ?, ?, ?, ?, 1)",
                              member.agent_id, member.name, member.agent_key, member.team_id, member.joined_at != 0 ? member.joined_at : now_millis());
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert team member: {}", ex.what());
        }
    }

    std::vector<TeamMemberRecord> TeamManager::list_members(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return sqlite::query_all<TeamMemberRecord>(
                impl_->db,
                "SELECT agent_id, name, agent_key, team_id, joined_at, active "
                "FROM team_members WHERE team_id = ? AND active = 1",
                team_id);
        } catch (const std::exception &ex) {
            spdlog::error("failed to list team members for {}: {}", team_id, ex.what());
            return {};
        }
    }

    std::vector<std::string> TeamManager::list_member_names(const std::string &team_id) const {
        auto members = list_members(team_id);
        std::vector<std::string> names;
        names.reserve(members.size());
        for (auto &member : members) {
            names.push_back(std::move(member.name));
        }
        return names;
    }

    void TeamManager::deactivate_member(const std::string &team_id, const std::string &agent_id) {
        std::scoped_lock lock(impl_->mutex);

        try {
            sqlite::exec_bind(impl_->db, "UPDATE team_members SET active = 0 WHERE team_id = ? AND agent_id = ?", team_id, agent_id);
        } catch (const std::exception &ex) {
            spdlog::error("failed to deactivate team member {} in {}: {}", agent_id, team_id, ex.what());
        }
    }

    void TeamManager::abandon_active_members(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        try {
            sqlite::exec_bind(impl_->db, "UPDATE team_members SET active = 0 WHERE team_id = ? AND active = 1", team_id);
        } catch (const std::exception &ex) {
            spdlog::error("failed to abandon active team members for {}: {}", team_id, ex.what());
        }
    }

    std::vector<TeamRecord> TeamManager::list_active_teams() const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return sqlite::query_all<TeamRecord>(
                impl_->db,
                "SELECT id, name, description, lead_agent_id, created_at, active FROM teams WHERE active = 1");
        } catch (const std::exception &ex) {
            spdlog::error("failed to list active teams: {}", ex.what());
            return {};
        }
    }

} // namespace orangutan::orchestration
