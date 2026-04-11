#pragma once

#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/cli-options.hpp"
#include "tools/registry/tool.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::automation {
    class Runtime;
}

namespace orangutan::config {
    struct Config;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::memory {
    class MemoryStore;
}

namespace orangutan::providers {
    class Provider;
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
    class WebServer;
}

namespace orangutan::bootstrap::detail {

    struct WebStartupInspection {
        bool has_session_store = false;
        bool has_memory_store = false;
        bool has_runtime_bundle = false;
        bool has_runtime_agent = false;
        bool attached_session_store = false;
        bool attached_tool_registry = false;
        bool attached_skill_loader = false;
        bool attached_config_save_path = false;
        std::vector<ToolDef> tool_definitions;
        std::vector<std::string> active_skill_names;
        std::string runtime_build_error;
    };

    using WebStartupInspectionCallback = std::function<bool(const WebStartupInspection &)>;
    using WebRuntimeBuildCallback = std::function<void()>;
    using ChannelModeCallback = std::function<int()>;

    ChannelModeCallback &channel_mode_callback();
    void reset_signal_stop_requested_for_tests();
    void set_web_startup_inspection_callback_for_tests(WebStartupInspectionCallback callback);
    void clear_web_startup_inspection_callback_for_tests();
    void set_web_runtime_build_callback_for_tests(WebRuntimeBuildCallback callback);
    void clear_web_runtime_build_callback_for_tests();
    void set_channel_mode_callback_for_tests(ChannelModeCallback callback);
    void clear_channel_mode_callback_for_tests();
    void maybe_inject_web_runtime_build_failure_for_tests();

    [[nodiscard]]
    bool inspect_web_startup_for_tests(const WebStartupInspection &inspection);

} // namespace orangutan::bootstrap::detail

namespace orangutan::bootstrap {

    struct WebServerRuntimeAttachments {
        bool session_store_attached = false;
        bool tool_registry_attached = false;
        bool skill_loader_attached = false;
        bool config_save_path_attached = false;
    };

    struct RuntimeCompletionResumeState {
        std::mutex mutex;
        agent::AgentLoop *agent = nullptr;
        providers::Provider *provider = nullptr;
        hooks::HookManager *hook_manager = nullptr;
        storage::SessionStore *session_store = nullptr;
        std::string *current_session_id = nullptr;
        std::string agent_key;
        std::string configured_model;
        std::string scope_key;
        automation::Runtime *automation_runtime = nullptr;
        bool persist_session = false;
        bool suppress_human_output = false;
    };

    std::atomic<bool> &signal_stop_requested();

    void handle_signal(int signum);

    [[nodiscard]]
    std::vector<std::string> collect_active_skill_names(const skills::SkillLoader *skill_loader);

    [[nodiscard]]
    detail::WebStartupInspection build_web_startup_inspection(storage::SessionStore *session_store, memory::MemoryStore *memory_store, const AgentRuntimeBundle *runtime,
                                                              skills::SkillLoader *skill_loader, const WebServerRuntimeAttachments &attachments,
                                                              std::string_view runtime_build_error);

    [[nodiscard]]
    bool maybe_skip_web_server_start_for_tests(storage::SessionStore *session_store, memory::MemoryStore *memory_store, const AgentRuntimeBundle *runtime,
                                               skills::SkillLoader *skill_loader, const WebServerRuntimeAttachments &attachments, std::string_view runtime_build_error = {});

    [[nodiscard]]
    WebServerRuntimeAttachments configure_web_server_runtime(web::WebServer &web_server, const CliOptions &options, config::Config &cfg, storage::SessionStore *session_store,
                                                             memory::MemoryStore *memory_store, automation::Runtime *automation_runtime = nullptr,
                                                             tools::ToolRegistry *tool_registry = nullptr, skills::SkillLoader *skill_loader = nullptr);

    void warn_if_nonlocal_web_host(const std::string &web_host);

    void deactivate_runtime_completion_resume_state(const std::shared_ptr<RuntimeCompletionResumeState> &state);

    [[nodiscard]]
    BackgroundCompletionResumeCallback make_runtime_completion_resume_callback(const std::weak_ptr<RuntimeCompletionResumeState> &weak_state);

    [[nodiscard]]
    std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_runtime_background_completion_bindings(automation::Runtime *automation_runtime,
                                                                                                           BackgroundCompletionResumeCallback resume_callback);

} // namespace orangutan::bootstrap
