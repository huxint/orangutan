#pragma once

#include "features/agent/agent-loop.hpp"
#include "core/providers/provider.hpp"
#include "core/tools/tool.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace orangutan {

struct WebSessionState {
    std::string session_id;
    std::unique_ptr<Provider> provider;
    std::unique_ptr<ToolRegistry> tools;
    std::unique_ptr<AgentLoop> agent;
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> running{false};

    WebSessionState() = default;
    ~WebSessionState() = default;
    WebSessionState(const WebSessionState &) = delete;
    WebSessionState &operator=(const WebSessionState &) = delete;
    WebSessionState(WebSessionState &&) = delete;
    WebSessionState &operator=(WebSessionState &&) = delete;
};

} // namespace orangutan
