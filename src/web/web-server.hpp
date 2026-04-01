#pragma once

#include "web/web-types.hpp"
#include <chrono>
#include <httplib.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace orangutan {

    class MemoryStore;
    class SessionStore;
    class SubagentManager;
    class ToolRegistry;
    class SkillLoader;
    struct Config;
    namespace automation {
        class Runtime;
    }

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

        void set_session_store(SessionStore *store);
        void set_memory_store(MemoryStore *store);
        void set_subagent_manager(SubagentManager *manager);
        void set_config(Config *config);
        void set_config_save_path(const std::filesystem::path &path);
        void set_tool_registry(ToolRegistry *registry);
        void set_skill_loader(SkillLoader *loader);
        void set_automation_runtime(automation::Runtime *runtime);

    private:
        httplib::Server server_;
        std::thread server_thread_;
        std::filesystem::path static_dir_;
        int port_ = 0;
        std::atomic<bool> running_{false};

        SessionStore *session_store_ = nullptr;
        MemoryStore *memory_store_ = nullptr;
        SubagentManager *subagent_manager_ = nullptr;
        Config *config_ = nullptr;
        std::filesystem::path config_save_path_;
        ToolRegistry *tool_registry_ = nullptr;
        SkillLoader *skill_loader_ = nullptr;
        automation::Runtime *automation_runtime_ = nullptr;
        std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

        std::mutex sessions_mutex_;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions_;

        void setup_routes();
    };

} // namespace orangutan
