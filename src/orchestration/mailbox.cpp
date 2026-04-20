#include "orchestration/mailbox.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "storage/sqlite-throwing.hpp"
#include "utils/enum-string.hpp"
#include "utils/scope-exit.hpp"

namespace orangutan::orchestration {

    namespace {

        constexpr auto INSERT_MESSAGE_SQL =
            "INSERT INTO agent_mailbox (id, team_id, sender, recipient, body, timestamp, is_read, message_type) VALUES (?, ?, ?, ?, ?, ?, 0, ?)";

        constexpr auto POLL_MESSAGES_SQL =
            "SELECT id, team_id, sender, recipient, body, timestamp, is_read, message_type "
            "FROM agent_mailbox WHERE team_id = ? AND recipient = ? AND is_read = 0 "
            "ORDER BY timestamp ASC";

        constexpr auto MARK_READ_SQL = "UPDATE agent_mailbox SET is_read = 1 WHERE id = ?";

        auto generate_id() -> std::string {
            static std::atomic<std::uint64_t> counter{0};
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
            return "msg-" + std::to_string(micros) + "-" + std::to_string(sequence);
        }

        auto now_millis() -> std::int64_t {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        auto read_mailbox_message(const sqlite::Row &row) -> MailboxMessage {
            return MailboxMessage{
                .id = sqlite::unwrap(row.get<std::string>(0)),
                .team_id = sqlite::unwrap(row.get<std::string>(1)),
                .from = sqlite::unwrap(row.get<std::string>(2)),
                .to = sqlite::unwrap(row.get<std::string>(3)),
                .text = sqlite::unwrap(row.get<std::string>(4)),
                .timestamp = sqlite::unwrap(row.get<std::int64_t>(5)),
                .read = sqlite::unwrap(row.get<int>(6)) != 0,
                .type = utils::parse_enum_or(sqlite::unwrap(row.get<std::string>(7)), message_type::message),
            };
        }

        void run_statement(sqlite::Statement &stmt) {
            const auto reset_stmt = utils::scope_exit([&stmt] {
                static_cast<void>(stmt.reset()); // NOLINT(bugprone-unused-return-value): scope-exit cleanup must not throw
            });
            while (sqlite::unwrap(stmt.step())) {
            }
        }

        void insert_mailbox_message(sqlite::Statement &stmt,
                                    std::string_view team_id,
                                    std::string_view from,
                                    std::string_view to,
                                    std::string_view text,
                                    message_type type) {
            sqlite::unwrap(stmt.clear_bindings());
            stmt.bind_all(generate_id(), team_id, from, to, text, now_millis(), utils::enum_name(type));
            run_statement(stmt);
        }

        void mark_mailbox_message_read(sqlite::Statement &stmt, std::string_view message_id) {
            sqlite::unwrap(stmt.clear_bindings());
            stmt.bind_all(message_id);
            run_statement(stmt);
        }

    } // namespace

    struct AgentMailbox::Impl {
        sqlite::Database db;
        std::mutex mutex;

        explicit Impl(const std::filesystem::path &db_path)
        : db(sqlite::open_or_throw(db_path)) {
            sqlite::exec_script(db, "PRAGMA journal_mode=WAL;", "failed to enable mailbox wal mode");
            sqlite::exec_script(db,
                                "CREATE TABLE IF NOT EXISTS agent_mailbox ("
                                "    id TEXT PRIMARY KEY,"
                                "    team_id TEXT NOT NULL,"
                                "    sender TEXT NOT NULL,"
                                "    recipient TEXT NOT NULL,"
                                "    body TEXT NOT NULL,"
                                "    timestamp INTEGER NOT NULL,"
                                "    is_read INTEGER NOT NULL DEFAULT 0,"
                                "    message_type TEXT NOT NULL DEFAULT 'message'"
                                ");",
                                "failed to create mailbox table");
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
            auto insert = sqlite::prepare_or_throw(impl_->db, INSERT_MESSAGE_SQL);
            insert_mailbox_message(insert, team_id, from, to, text, type);
        } catch (const std::exception &ex) {
            spdlog::error("failed to insert mailbox message: {}", ex.what());
        }
    }

    void AgentMailbox::send_broadcast(const std::string &team_id, const std::string &from, const std::string &text, const std::vector<std::string> &team_members) {
        std::scoped_lock lock(impl_->mutex);
        try {
            sqlite::unwrap(impl_->db.transaction([&](sqlite::Database &tx) {
                auto insert = sqlite::prepare_or_throw(tx, INSERT_MESSAGE_SQL);
                for (const auto &member : team_members) {
                    if (member == from) {
                        continue;
                    }
                    try {
                        insert_mailbox_message(insert, team_id, from, member, text, message_type::message);
                    } catch (const std::exception &ex) {
                        spdlog::error("failed to insert broadcast mailbox message for {}: {}", member, ex.what());
                    }
                }
            }));
        } catch (const std::exception &ex) {
            spdlog::error("failed to broadcast mailbox messages: {}", ex.what());
        }
    }

    std::vector<MailboxMessage> AgentMailbox::poll(const std::string &team_id, const std::string &agent_name) {
        std::scoped_lock lock(impl_->mutex);
        try {
            std::vector<MailboxMessage> messages;
            auto query = sqlite::unwrap(impl_->db.query(POLL_MESSAGES_SQL));
            sqlite::unwrap(query.bind(team_id, agent_name).for_each([&messages](const sqlite::Row &row) {
                messages.push_back(read_mailbox_message(row));
            }));
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
            sqlite::unwrap(impl_->db.transaction([&](sqlite::Database &tx) {
                auto update = sqlite::prepare_or_throw(tx, MARK_READ_SQL);
                for (const auto &message_id : message_ids) {
                    try {
                        mark_mailbox_message_read(update, message_id);
                    } catch (const std::exception &ex) {
                        spdlog::error("failed to mark mailbox message read for {}: {}", message_id, ex.what());
                    }
                }
            }));
        } catch (const std::exception &ex) {
            spdlog::error("failed to batch mark mailbox messages read: {}", ex.what());
        }
    }

    void AgentMailbox::clear_team(const std::string &team_id) {
        std::scoped_lock lock(impl_->mutex);
        try {
            sqlite::exec_bind(impl_->db, "DELETE FROM agent_mailbox WHERE team_id = ?", team_id);
        } catch (const std::exception &ex) {
            spdlog::error("failed to clear team mailbox: {}", ex.what());
        }
    }

} // namespace orangutan::orchestration
