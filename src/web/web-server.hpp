#pragma once

#include "web/web-types.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <httplib.h>

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

    struct WebSessionState;

    class WebServer {
    public:
        WebServer();
        ~WebServer();

        WebServer(const WebServer &) = delete;
        WebServer &operator=(const WebServer &) = delete;
        WebServer(WebServer &&) = delete;
        WebServer &operator=(WebServer &&) = delete;

        void start(const std::string &host = "127.0.0.1", int port = 18080);
        void stop();

        [[nodiscard]]
        bool is_running() const;
        [[nodiscard]]
        int port() const;

        void set_static_dir(const std::filesystem::path &path);

        void set_session_store(storage::SessionStore *store);
        void set_memory_store(memory::MemoryStore *store);
        void set_config(config::Config *config);
        void set_config_save_path(const std::filesystem::path &path);
        void set_tool_registry(tools::ToolRegistry *registry);
        void set_skill_loader(skills::SkillLoader *loader);
        void set_automation_service(automation::AutomationService *service);
        void set_automation_runtime(automation::AutomationRuntime *runtime);

    private:
        httplib::Server server_;
        std::jthread server_thread_;
        std::filesystem::path static_dir_;
        int port_ = 0;
        std::atomic<bool> running_{false};

        std::mutex startup_mutex_;
        std::condition_variable startup_cv_;
        bool startup_complete_ = false;

        storage::SessionStore *session_store_ = nullptr;
        memory::MemoryStore *memory_store_ = nullptr;
        config::Config *config_ = nullptr;
        std::filesystem::path config_save_path_;
        tools::ToolRegistry *tool_registry_ = nullptr;
        skills::SkillLoader *skill_loader_ = nullptr;
        automation::AutomationService *automation_service_ = nullptr;
        automation::AutomationRuntime *automation_runtime_ = nullptr;
        std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

        std::mutex sessions_mutex_;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions_;

        void setup_routes();
    };

} // namespace orangutan::web

namespace orangutan {

    using web::WebServer;

} // namespace orangutan
