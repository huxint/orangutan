#include "swarm/mailbox.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace orangutan::swarm {

    namespace {

        std::string message_type_to_string(MessageType type) {
            switch (type) {
                case MessageType::message:
                    return "message";
                case MessageType::shutdown_request:
                    return "shutdown_request";
                case MessageType::shutdown_response:
                    return "shutdown_response";
            }
            return "message";
        }

        MessageType string_to_message_type(const std::string &s) {
            if (s == "shutdown_request") {
                return MessageType::shutdown_request;
            }
            if (s == "shutdown_response") {
                return MessageType::shutdown_response;
            }
            return MessageType::message;
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

    } // namespace

    struct AgentMailbox::Impl {
        sqlite3 *db = nullptr;
        std::mutex mutex;

        explicit Impl(const std::string &db_path) {
            int rc = sqlite3_open(db_path.c_str(), &db);
            if (rc != SQLITE_OK) {
                std::string err = sqlite3_errmsg(db);
                sqlite3_close(db);
                db = nullptr;
                throw std::runtime_error("Failed to open mailbox database: " + err);
            }

            // Enable WAL mode
            sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

            const char *create_sql = "CREATE TABLE IF NOT EXISTS agent_mailbox ("
                                     "    id TEXT PRIMARY KEY,"
                                     "    team_id TEXT NOT NULL,"
                                     "    sender TEXT NOT NULL,"
                                     "    recipient TEXT NOT NULL,"
                                     "    body TEXT NOT NULL,"
                                     "    timestamp INTEGER NOT NULL,"
                                     "    is_read INTEGER NOT NULL DEFAULT 0,"
                                     "    message_type TEXT NOT NULL DEFAULT 'message'"
                                     ");";

            char *err_msg = nullptr;
            rc = sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg);
            if (rc != SQLITE_OK) {
                std::string err = err_msg != nullptr ? err_msg : "unknown error";
                sqlite3_free(err_msg);
                throw std::runtime_error("Failed to create mailbox table: " + err);
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

    AgentMailbox::AgentMailbox(const std::string &db_path)
    : impl_(std::make_unique<Impl>(db_path)) {}

    AgentMailbox::~AgentMailbox() = default;

    void AgentMailbox::send(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text, MessageType type) {
        std::lock_guard lock(impl_->mutex);

        const char *sql = "INSERT INTO agent_mailbox (id, team_id, sender, recipient, body, timestamp, is_read, message_type) VALUES (?, ?, ?, ?, ?, ?, 0, ?);";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("Failed to prepare mailbox insert: {}", sqlite3_errmsg(impl_->db));
            return;
        }

        auto id = generate_id();
        auto ts = now_millis();
        auto type_str = message_type_to_string(type);

        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, team_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, from.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, to.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, ts);
        sqlite3_bind_text(stmt, 7, type_str.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            spdlog::error("Failed to insert mailbox message: {}", sqlite3_errmsg(impl_->db));
        }
        sqlite3_finalize(stmt);
    }

    void AgentMailbox::send_broadcast(const std::string &team_id, const std::string &from, const std::string &text, const std::vector<std::string> &team_members) {
        for (const auto &member : team_members) {
            if (member != from) {
                send(team_id, from, member, text, MessageType::message);
            }
        }
    }

    std::vector<MailboxMessage> AgentMailbox::poll(const std::string &team_id, const std::string &agent_name) {
        std::lock_guard lock(impl_->mutex);

        const char *sql = "SELECT id, team_id, sender, recipient, body, timestamp, is_read, message_type "
                          "FROM agent_mailbox WHERE team_id = ? AND recipient = ? AND is_read = 0 "
                          "ORDER BY timestamp ASC;";

        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("Failed to prepare mailbox poll: {}", sqlite3_errmsg(impl_->db));
            return {};
        }

        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, agent_name.c_str(), -1, SQLITE_TRANSIENT);

        std::vector<MailboxMessage> messages;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MailboxMessage msg;
            msg.id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            msg.team_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            msg.from = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
            msg.to = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            msg.text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
            msg.timestamp = sqlite3_column_int64(stmt, 5);
            msg.read = sqlite3_column_int(stmt, 6) != 0;
            msg.type = string_to_message_type(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7)));
            messages.push_back(std::move(msg));
        }

        sqlite3_finalize(stmt);
        return messages;
    }

    void AgentMailbox::mark_read(const std::vector<std::string> &message_ids) {
        if (message_ids.empty()) {
            return;
        }

        std::lock_guard lock(impl_->mutex);

        for (const auto &id : message_ids) {
            const char *sql = "UPDATE agent_mailbox SET is_read = 1 WHERE id = ?;";
            sqlite3_stmt *stmt = nullptr;
            int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                spdlog::error("Failed to prepare mark_read: {}", sqlite3_errmsg(impl_->db));
                continue;
            }
            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                spdlog::error("Failed to mark message read: {}", sqlite3_errmsg(impl_->db));
            }
            sqlite3_finalize(stmt);
        }
    }

    void AgentMailbox::clear_team(const std::string &team_id) {
        std::lock_guard lock(impl_->mutex);

        const char *sql = "DELETE FROM agent_mailbox WHERE team_id = ?;";
        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("Failed to prepare clear_team: {}", sqlite3_errmsg(impl_->db));
            return;
        }
        sqlite3_bind_text(stmt, 1, team_id.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            spdlog::error("Failed to clear team mailbox: {}", sqlite3_errmsg(impl_->db));
        }
        sqlite3_finalize(stmt);
    }

} // namespace orangutan::swarm
