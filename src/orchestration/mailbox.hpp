#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "types/base.hpp"

namespace orangutan::orchestration {

    enum class message_type : base::u8 {
        message,
        shutdown_request,
        shutdown_response,
    };

    struct MailboxMessage {
        std::string id;
        std::string team_id;
        std::string from;
        std::string to; // agent name or "*" for broadcast
        std::string text;
        std::int64_t timestamp = 0;
        bool read = false;
        message_type type = message_type::message;
    };

    class AgentMailbox {
    public:
        explicit AgentMailbox(const std::filesystem::path &db_path);
        ~AgentMailbox();

        AgentMailbox(const AgentMailbox &) = delete;
        AgentMailbox &operator=(const AgentMailbox &) = delete;
        AgentMailbox(AgentMailbox &&) = delete;
        AgentMailbox &operator=(AgentMailbox &&) = delete;

        void send(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text, message_type type = message_type::message);

        void send_broadcast(const std::string &team_id, const std::string &from, const std::string &text, const std::vector<std::string> &team_members);

        [[nodiscard]]
        std::vector<MailboxMessage> poll(const std::string &team_id, const std::string &agent_name);

        void mark_read(const std::vector<std::string> &message_ids);

        void clear_team(const std::string &team_id);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::AgentMailbox;
    using orchestration::MailboxMessage;
    using orchestration::message_type;
} // namespace orangutan
