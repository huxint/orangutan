#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orangutan::swarm {

    enum class MessageType {
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
        MessageType type = MessageType::message;
    };

    class AgentMailbox {
    public:
        explicit AgentMailbox(const std::string &db_path);
        ~AgentMailbox();

        AgentMailbox(const AgentMailbox &) = delete;
        AgentMailbox &operator=(const AgentMailbox &) = delete;
        AgentMailbox(AgentMailbox &&) = delete;
        AgentMailbox &operator=(AgentMailbox &&) = delete;

        void send(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text, MessageType type = MessageType::message);

        void send_broadcast(const std::string &team_id, const std::string &from, const std::string &text, const std::vector<std::string> &team_members);

        [[nodiscard]]
        std::vector<MailboxMessage> poll(const std::string &team_id, const std::string &agent_name);

        void mark_read(const std::vector<std::string> &message_ids);

        void clear_team(const std::string &team_id);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace orangutan::swarm

namespace orangutan {
    using swarm::AgentMailbox;
    using swarm::MailboxMessage;
    using swarm::MessageType;
} // namespace orangutan
