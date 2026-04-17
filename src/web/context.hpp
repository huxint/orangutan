#pragma once

#include "web/web-types.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace orangutan::automation {
    class AutomationRuntime;
    class AutomationService;
}

namespace orangutan::config {
    struct Config;
}

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::tools {
    class ToolRegistry;
}

namespace orangutan::web {

    class EventBus;

    /// Aggregated non-owning references to every dependency a web handler needs.
    /// Passed by const-reference into route functions so handler signatures stay
    /// uniform and there is no 8-argument call gymnastics.
    struct WebContext {
        config::Config *config = nullptr;
        std::filesystem::path config_save_path;
        storage::SessionStore *session_store = nullptr;
        memory::MemoryStore *memory_store = nullptr;
        tools::ToolRegistry *tool_registry = nullptr;
        skills::SkillLoader *skill_loader = nullptr;
        automation::AutomationService *automation_service = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
        EventBus *event_bus = nullptr;

        std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

        std::mutex *sessions_mutex = nullptr;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> *sessions = nullptr;
    };

} // namespace orangutan::web
