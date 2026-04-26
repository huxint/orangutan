#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "orchestration/types.hpp"

namespace orangutan::orchestration {

    struct TeamRecord {
        std::string id;
        std::string name;
        std::string description;
        std::string lead_agent_id;
        std::int64_t created_at = 0;
        bool active = true;
    };

    struct TeamMemberRecord {
        std::string agent_id;
        std::string name;
        std::string config_agent_key;
        std::string team_id;
        teammate_relationship relationship = teammate_relationship::managed;
        std::int64_t joined_at = 0;
        bool active = true;
    };

    class TeamManager {
    public:
        explicit TeamManager(const std::filesystem::path &db_path);
        ~TeamManager();

        TeamManager(const TeamManager &) = delete;
        TeamManager &operator=(const TeamManager &) = delete;
        TeamManager(TeamManager &&) = delete;
        TeamManager &operator=(TeamManager &&) = delete;

        [[nodiscard]]
        TeamRecord create_team(const std::string &name,
                               const std::string &description,
                               const std::string &lead_agent_id);

        [[nodiscard]]
        std::optional<TeamRecord> find_team(const std::string &team_id) const;

        [[nodiscard]]
        std::optional<TeamRecord> find_team_by_name(const std::string &name) const;

        [[nodiscard]]
        std::optional<TeamRecord> find_team_for_lead(const std::string &lead_agent_id) const;

        void delete_team(const std::string &team_id);

        void add_member(const TeamMemberRecord &member);

        [[nodiscard]]
        std::vector<TeamMemberRecord> list_members(const std::string &team_id) const;

        [[nodiscard]]
        std::vector<std::string> list_member_names(const std::string &team_id) const;

        void deactivate_member(const std::string &team_id, const std::string &agent_id);

        void abandon_active_members(const std::string &team_id);

        [[nodiscard]]
        std::vector<TeamRecord> list_active_teams() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::TeamManager;
    using orchestration::TeamMemberRecord;
    using orchestration::TeamRecord;
} // namespace orangutan
