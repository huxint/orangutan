#pragma once

#include "cli/slash-commands.hpp"
#include "config/config.hpp"
#include "storage/session-store.hpp"
#include "web/context.hpp"
#include "web/web-routes.hpp"
#include "web/web-types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orangutan::web {

    enum class ChatSessionStartStatus : std::uint8_t {
        stream_ready,
        command_handled,
        session_active,
    };

    struct ChatSessionStartRequest {
        std::string message;
        std::string agent_key;
        config::AgentConfig agent;
        storage::SessionMetadata metadata;
        std::string session_id;
    };

    class ActiveChatSession {
    public:
        ~ActiveChatSession();

        ActiveChatSession(const ActiveChatSession &) = delete;
        ActiveChatSession &operator=(const ActiveChatSession &) = delete;
        ActiveChatSession(ActiveChatSession &&) = delete;
        ActiveChatSession &operator=(ActiveChatSession &&) = delete;

        [[nodiscard]]
        const std::string &session_id() const noexcept;

        [[nodiscard]]
        const std::string &agent_key() const noexcept;

        [[nodiscard]]
        const std::string &message() const noexcept;

        [[nodiscard]]
        const storage::SessionMetadata &metadata() const noexcept;

        [[nodiscard]]
        WebSessionState *session() const noexcept;

        [[nodiscard]]
        agent::AgentLoop *agent() const noexcept;

        void cleanup();

    private:
        friend class ChatSessionController;

        ActiveChatSession(const WebContext &ctx, std::string session_id, std::string agent_key, std::string message, storage::SessionMetadata metadata, WebSessionState *session,
                          std::shared_ptr<detail::web_approval_event_emitter> approval_event_emitter, std::shared_ptr<std::function<bool()>> approval_stream_open);

        std::mutex *sessions_mutex_ = nullptr;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> *sessions_ = nullptr;
        std::string session_id_;
        std::string agent_key_;
        std::string message_;
        storage::SessionMetadata metadata_;
        WebSessionState *session_ = nullptr;
        std::unique_ptr<WebSessionState> detached_session_;
        std::shared_ptr<detail::web_approval_event_emitter> approval_event_emitter_;
        std::shared_ptr<std::function<bool()>> approval_stream_open_;
        bool cleaned_up_ = false;
    };

    struct ChatSessionStartResult {
        ChatSessionStartStatus status = ChatSessionStartStatus::stream_ready;
        std::shared_ptr<ActiveChatSession> active_session;
        cli::SlashCommandReply command_response;
    };

    struct ChatSessionStreamCallbacks {
        std::function<bool(const std::string &session_id)> session;
        std::function<bool(const std::string &text)> text;
        std::function<bool(const std::string &thinking)> thinking;
        std::function<bool(const ToolUse &call)> tool_start;
        std::function<bool(const ToolUse &call, const ToolResult &result)> tool_end;
        std::function<bool()> done;
        std::function<bool(std::string_view error)> error;
        std::function<void()> complete;
        detail::web_approval_event_emitter approval_event_emitter;
        std::function<bool()> stream_open;
    };

    class ChatSessionController {
    public:
        explicit ChatSessionController(const WebContext &ctx);

        [[nodiscard]]
        ChatSessionStartResult start(const ChatSessionStartRequest &request) const;

        void stream(const std::shared_ptr<ActiveChatSession> &active_session, ChatSessionStreamCallbacks callbacks) const;

    private:
        const WebContext *ctx_ = nullptr;
    };

} // namespace orangutan::web
