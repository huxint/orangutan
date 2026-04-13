#include "swarm/team-manager.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "storage/sqlite.hpp"

namespace orangutan::sqlite {

    template <>
    struct RowMapper<orangutan::swarm::TeamRecord> {
        static auto map(const Row &row) -> orangutan::swarm::TeamRecord {
            return orangutan::swarm::TeamRecord{
                .id = row.get<std::string>(0),
                .name = row.get<std::string>(1),
                .description = row.get<std::string>(2),
                .lead_agent_id = row.get<std::string>(3),
                .created_at = row.get<std::int64_t>(4),
                .active = row.get<int>(5) != 0,
            };
        }
    };

    template <>
    struct RowMapper<orangutan::swarm::TeamMemberRecord> {
        static auto map(const Row &row) -> orangutan::swarm::TeamMemberRecord {
            return orangutan::swarm::TeamMemberRecord{
                .agent_id = row.get<std::string>(0),
                .name = row.get<std::string>(1),
                .agent_key = row.get<std::string>(2),
                .team_id = row.get<std::string>(3),
                .joined_at = row.get<std::int64_t>(4),
                .active = row.get<int>(5) != 0,
            };
        }
    };

} // namespace orangutan::sqlite

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
        sqlite::Database db;
        mutable std::mutex mutex;

        explicit Impl(const std::filesystem::path &db_path)
        : db(db_path) {
            db.exec_script("PRAGMA journal_mode=WAL;", "failed to enable team manager WAL mode");
            db.exec_script("CREATE TABLE IF NOT EXISTS teams ("
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
            impl_->db.exec("INSERT INTO teams (id, name, description, lead_agent_id, created_at, active) VALUES (?, ?, ?, ?, ?, 1)")
                .bind(record.id, name, description, lead_agent_id, record.created_at)
                .run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert team: {}", ex.what());
        }

        return record;
    }

    std::optional<TeamRecord> TeamManager::find_team(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return impl_->db.query("SELECT id, name, description, lead_agent_id, created_at, active "
                                   "FROM teams WHERE id = ?")
                .bind(team_id)
                .optional<TeamRecord>();
        } catch (const std::exception &ex) {
            spdlog::error("failed to find team {}: {}", team_id, ex.what());
            return std::nullopt;
        }
    }

    std::optional<TeamRecord> TeamManager::find_team_by_name(const std::string &name) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return impl_->db.query("SELECT id, name, description, lead_agent_id, created_at, active "
                                   "FROM teams WHERE name = ?")
                .bind(name)
                .optional<TeamRecord>();
        } catch (const std::exception &ex) {
            spdlog::error("failed to find team by name {}: {}", name, ex.what());
            return std::nullopt;
        }
    }

    void TeamManager::delete_team(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        try {
            impl_->db.transaction([&](sqlite::Database &tx) {
                tx.exec("DELETE FROM team_members WHERE team_id = ?").bind(team_id).run();
                tx.exec("DELETE FROM teams WHERE id = ?").bind(team_id).run();
            });
        } catch (const std::exception &ex) {
            spdlog::error("failed to delete team {}: {}", team_id, ex.what());
        }
    }

    void TeamManager::add_member(const TeamMemberRecord &member) {
        std::scoped_lock lock(impl_->mutex);

        try {
            impl_->db.exec("INSERT INTO team_members (agent_id, name, agent_key, team_id, joined_at, active) VALUES (?, ?, ?, ?, ?, 1)")
                .bind(member.agent_id, member.name, member.agent_key, member.team_id, member.joined_at != 0 ? member.joined_at : now_millis())
                .run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert team member: {}", ex.what());
        }
    }

    std::vector<TeamMemberRecord> TeamManager::list_members(const std::string &team_id) const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return impl_->db.query("SELECT agent_id, name, agent_key, team_id, joined_at, active "
                                   "FROM team_members WHERE team_id = ? AND active = 1")
                .bind(team_id)
                .all<TeamMemberRecord>();
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
            impl_->db.exec("UPDATE team_members SET active = 0 WHERE team_id = ? AND agent_id = ?").bind(team_id, agent_id).run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to deactivate team member {} in {}: {}", agent_id, team_id, ex.what());
        }
    }

    void TeamManager::abandon_active_members(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);

        try {
            impl_->db.exec("UPDATE team_members SET active = 0 WHERE team_id = ? AND active = 1").bind(team_id).run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to abandon active team members for {}: {}", team_id, ex.what());
        }
    }

    std::vector<TeamRecord> TeamManager::list_active_teams() const {
        std::scoped_lock lock(impl_->mutex);

        try {
            return impl_->db.query("SELECT id, name, description, lead_agent_id, created_at, active "
                                   "FROM teams WHERE active = 1")
                .all<TeamRecord>();
        } catch (const std::exception &ex) {
            spdlog::error("failed to list active teams: {}", ex.what());
            return {};
        }
    }

} // namespace orangutan::swarm
