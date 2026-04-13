#include "swarm/mailbox.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <tuple>

#include <spdlog/spdlog.h>

#include "storage/sqlite.hpp"

namespace orangutan::swarm {

    namespace {

        std::string message_type_to_string(message_type type) {
            switch (type) {
                case message_type::message:
                    return "message";
                case message_type::shutdown_request:
                    return "shutdown_request";
                case message_type::shutdown_response:
                    return "shutdown_response";
            }
            return "message";
        }

        message_type string_to_message_type(const std::string &s) {
            if (s == "shutdown_request") {
                return message_type::shutdown_request;
            }
            if (s == "shutdown_response") {
                return message_type::shutdown_response;
            }
            return message_type::message;
        }

        std::string generate_id() {
            static std::atomic<std::uint64_t> counter{0};
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            auto seq = counter.fetch_add(1, std::memory_order_relaxed);
            return "msg-" + std::to_string(ms) + "-" + std::to_string(seq);
        }

        std::int64_t now_millis() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        using mailbox_row = std::tuple<std::string, std::string, std::string, std::string, std::string, std::int64_t, int, std::string>;

        auto read_mailbox_message(const mailbox_row &row) -> MailboxMessage {
            return MailboxMessage{
                .id = std::get<0>(row),
                .team_id = std::get<1>(row),
                .from = std::get<2>(row),
                .to = std::get<3>(row),
                .text = std::get<4>(row),
                .timestamp = std::get<5>(row),
                .read = std::get<6>(row) != 0,
                .type = string_to_message_type(std::get<7>(row)),
            };
        }

    } // namespace

    struct AgentMailbox::Impl {
        sqlite::Database db;
        std::mutex mutex;

        explicit Impl(const std::filesystem::path &db_path)
        : db(db_path) {
            db.exec_script("PRAGMA journal_mode=WAL;", "Failed to enable mailbox WAL mode");
            db.exec_script("CREATE TABLE IF NOT EXISTS agent_mailbox ("
                           "    id TEXT PRIMARY KEY,"
                           "    team_id TEXT NOT NULL,"
                           "    sender TEXT NOT NULL,"
                           "    recipient TEXT NOT NULL,"
                           "    body TEXT NOT NULL,"
                           "    timestamp INTEGER NOT NULL,"
                           "    is_read INTEGER NOT NULL DEFAULT 0,"
                           "    message_type TEXT NOT NULL DEFAULT 'message'"
                           ");",
                           "Failed to create mailbox table");
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;

        ~Impl() = default;
    };

    AgentMailbox::AgentMailbox(const std::filesystem::path &db_path)
    : impl_(std::make_unique<Impl>(db_path)) {}

    AgentMailbox::~AgentMailbox() = default;

    void AgentMailbox::send(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text, message_type type) {
        std::scoped_lock lock(impl_->mutex);
        try {
            auto id = generate_id();
            auto ts = now_millis();
            auto type_str = message_type_to_string(type);

            impl_->db.exec("INSERT INTO agent_mailbox (id, team_id, sender, recipient, body, timestamp, is_read, message_type) VALUES (?, ?, ?, ?, ?, ?, 0, ?)")
                .bind(id, team_id, from, to, text, ts, type_str)
                .run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert mailbox message: {}", ex.what());
        }
    }

    void AgentMailbox::send_broadcast(const std::string &team_id, const std::string &from, const std::string &text, const std::vector<std::string> &team_members) {
        for (const auto &member : team_members) {
            if (member != from) {
                send(team_id, from, member, text, message_type::message);
            }
        }
    }

    std::vector<MailboxMessage> AgentMailbox::poll(const std::string &team_id, const std::string &agent_name) {
        std::scoped_lock lock(impl_->mutex);
        try {
            std::vector<MailboxMessage> messages;
            for (const auto &row : impl_->db.query("SELECT id, team_id, sender, recipient, body, timestamp, is_read, message_type "
                                                   "FROM agent_mailbox WHERE team_id = ? AND recipient = ? AND is_read = 0 "
                                                   "ORDER BY timestamp ASC")
                                       .bind(team_id, agent_name)
                                       .all<mailbox_row>()) {
                messages.push_back(read_mailbox_message(row));
            }
            return messages;
        } catch (const std::exception &ex) {
            spdlog::error("failed to poll mailbox: {}", ex.what());
            return {};
        }
    }

    void AgentMailbox::mark_read(const std::vector<std::string> &message_ids) {
        if (message_ids.empty()) {
            return;
        }

        std::scoped_lock lock(impl_->mutex);
        try {
            sqlite::Statement stmt(impl_->db, "UPDATE agent_mailbox SET is_read = 1 WHERE id = ?");
            for (const auto &id : message_ids) {
                try {
                    stmt.clear_bindings();
                    stmt.bind(1, id);
                    static_cast<void>(stmt.step());
                    stmt.reset();
                } catch (const std::exception &ex) {
                    spdlog::error("failed to mark message read: {}", ex.what());
                    stmt.reset();
                }
            }
        } catch (const std::exception &ex) {
            spdlog::error("failed to prepare mark_read: {}", ex.what());
        }
    }

    void AgentMailbox::clear_team(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);
        try {
            impl_->db.exec("DELETE FROM agent_mailbox WHERE team_id = ?").bind(team_id).run();
        } catch (const std::exception &ex) {
            spdlog::error("failed to clear team mailbox: {}", ex.what());
        }
    }

} // namespace orangutan::swarm
