#pragma once

#include "app/runtime/agent-runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace orangutan {

struct WebCompletionResumeState {
    std::mutex mutex;
    AgentLoop *agent = nullptr;
    std::string agent_key;
    automation::Runtime *automation_runtime = nullptr;
};

struct WebPendingApproval {
    std::string request_id;
    std::string tool;
    std::optional<std::string> command;
    std::string sandbox_mode;
    std::string prompt;
    std::chrono::steady_clock::time_point requested_at = std::chrono::steady_clock::now();
    bool resolved = false;
    bool approved = false;
    bool cancelled = false;
    std::mutex mutex;
    std::condition_variable condition;
};

struct WebSessionState {
    std::string session_id;
    std::unique_ptr<AgentRuntimeBundle> runtime;
    std::shared_ptr<WebPendingApproval> pending_approval;
    std::shared_ptr<WebCompletionResumeState> completion_resume_state;
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> running{false};

    [[nodiscard]]
    AgentLoop *agent() noexcept {
        return runtime != nullptr ? runtime->agent.get() : nullptr;
    }

    [[nodiscard]]
    const AgentLoop *agent() const noexcept {
        return runtime != nullptr ? runtime->agent.get() : nullptr;
    }

    [[nodiscard]]
    ToolRegistry *tools() noexcept {
        return runtime != nullptr ? &runtime->tools : nullptr;
    }

    [[nodiscard]]
    const ToolRegistry *tools() const noexcept {
        return runtime != nullptr ? &runtime->tools : nullptr;
    }

    WebSessionState() = default;
    ~WebSessionState() = default;
    WebSessionState(const WebSessionState &) = delete;
    WebSessionState &operator=(const WebSessionState &) = delete;
    WebSessionState(WebSessionState &&) = delete;
    WebSessionState &operator=(WebSessionState &&) = delete;
};

} // namespace orangutan
