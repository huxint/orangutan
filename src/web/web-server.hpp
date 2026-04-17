#pragma once

#include "web/context.hpp"
#include "web/event-bus.hpp"
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

    class WebServerBuilder;

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

        /// Create a builder for fluent WebServer configuration.
        [[nodiscard]]
        static WebServerBuilder configure(WebServer &server);

    private:
        httplib::Server server_;
        std::jthread server_thread_;
        std::filesystem::path static_dir_;
        int port_ = 0;
        std::atomic<bool> running_{false};

        std::mutex startup_mutex_;
        std::condition_variable startup_cv_;
        bool startup_complete_ = false;

        std::mutex sessions_mutex_;
        std::unordered_map<std::string, std::unique_ptr<WebSessionState>> sessions_;
        EventBus event_bus_;

        WebContext context_;

        void setup_routes();
    };

    class WebServerBuilder {
    public:
        explicit WebServerBuilder(WebServer &server) : server_(server) {}

        auto with_static_dir(this auto &&self, const std::filesystem::path &path) -> decltype(auto) {
            self.server_.set_static_dir(path);
            return std::forward<decltype(self)>(self);
        }

        auto with_session_store(this auto &&self, storage::SessionStore *store) -> decltype(auto) {
            self.server_.set_session_store(store);
            return std::forward<decltype(self)>(self);
        }

        auto with_memory_store(this auto &&self, memory::MemoryStore *store) -> decltype(auto) {
            self.server_.set_memory_store(store);
            return std::forward<decltype(self)>(self);
        }

        auto with_config(this auto &&self, config::Config *config) -> decltype(auto) {
            self.server_.set_config(config);
            return std::forward<decltype(self)>(self);
        }

        auto with_config_save_path(this auto &&self, const std::filesystem::path &path) -> decltype(auto) {
            self.server_.set_config_save_path(path);
            return std::forward<decltype(self)>(self);
        }

        auto with_tool_registry(this auto &&self, tools::ToolRegistry *registry) -> decltype(auto) {
            self.server_.set_tool_registry(registry);
            return std::forward<decltype(self)>(self);
        }

        auto with_skill_loader(this auto &&self, skills::SkillLoader *loader) -> decltype(auto) {
            self.server_.set_skill_loader(loader);
            return std::forward<decltype(self)>(self);
        }

        auto with_automation_service(this auto &&self, automation::AutomationService *service) -> decltype(auto) {
            self.server_.set_automation_service(service);
            return std::forward<decltype(self)>(self);
        }

        auto with_automation_runtime(this auto &&self, automation::AutomationRuntime *runtime) -> decltype(auto) {
            self.server_.set_automation_runtime(runtime);
            return std::forward<decltype(self)>(self);
        }

        /// Terminal: returns a reference to the configured server.
        [[nodiscard]]
        WebServer &build() const { return server_; }

    private:
        WebServer &server_;
    };

} // namespace orangutan::web

namespace orangutan {

    using web::WebServer;

} // namespace orangutan
