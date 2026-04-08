#include "bootstrap/runtime-control.hpp"

#include "agent/agent-loop.hpp"
#include "automation/scheduler.hpp"
#include "cli/single-shot.hpp"
#include "cli/session-workflow.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/memory-store.hpp"
#include "providers/provider.hpp"
#include "skills/skill-loader.hpp"
#include "storage/session-store.hpp"
#include "web/web-server.hpp"
#include "config/config.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap::detail {

    namespace {

        WebStartupInspectionCallback &web_startup_inspection_callback() {
            static WebStartupInspectionCallback callback;
            return callback;
        }

        WebRuntimeBuildCallback &web_runtime_build_callback() {
            static WebRuntimeBuildCallback callback;
            return callback;
        }

    } // namespace

    ChannelModeCallback &channel_mode_callback() {
        static ChannelModeCallback callback;
        return callback;
    }

    void reset_signal_stop_requested_for_tests() {
        signal_stop_requested().store(false);
    }

    void set_web_startup_inspection_callback_for_tests(WebStartupInspectionCallback callback) {
        web_startup_inspection_callback() = std::move(callback);
    }

    void clear_web_startup_inspection_callback_for_tests() {
        web_startup_inspection_callback() = {};
    }

    void set_web_runtime_build_callback_for_tests(WebRuntimeBuildCallback callback) {
        web_runtime_build_callback() = std::move(callback);
    }

    void clear_web_runtime_build_callback_for_tests() {
        web_runtime_build_callback() = {};
    }

    void set_channel_mode_callback_for_tests(ChannelModeCallback callback) {
        channel_mode_callback() = std::move(callback);
    }

    void clear_channel_mode_callback_for_tests() {
        channel_mode_callback() = {};
    }

    void maybe_inject_web_runtime_build_failure_for_tests() {
        auto &callback = web_runtime_build_callback();
        if (callback != nullptr) {
            callback();
        }
    }

    bool inspect_web_startup_for_tests(const WebStartupInspection &inspection) {
        auto &callback = web_startup_inspection_callback();
        if (callback == nullptr) {
            return false;
        }
        return callback(inspection);
    }

} // namespace orangutan::bootstrap::detail

namespace orangutan::bootstrap {

    std::atomic<bool> &signal_stop_requested() {
        static std::atomic<bool> stop_requested{false};
        return stop_requested;
    }

    void handle_signal(int /*signum*/) {
        signal_stop_requested().store(true);
    }

    std::vector<std::string> collect_active_skill_names(const skills::SkillLoader *skill_loader) {
        if (skill_loader == nullptr) {
            return {};
        }

        std::vector<std::string> skill_names;
        skill_names.reserve(skill_loader->active_skills().size());
        for (const auto &skill : skill_loader->active_skills()) {
            skill_names.push_back(skill.name);
        }
        return skill_names;
    }

    detail::WebStartupInspection build_web_startup_inspection(storage::SessionStore *session_store, memory::MemoryStore *memory_store, const AgentRuntimeBundle *runtime,
                                                              skills::SkillLoader *skill_loader, const WebServerRuntimeAttachments &attachments,
                                                              std::string_view runtime_build_error) {
        detail::WebStartupInspection inspection;
        inspection.has_session_store = session_store != nullptr;
        inspection.has_memory_store = memory_store != nullptr;
        inspection.has_runtime_bundle = runtime != nullptr;
        inspection.has_runtime_agent = runtime != nullptr && runtime->agent != nullptr;
        inspection.attached_session_store = attachments.session_store_attached;
        inspection.attached_tool_registry = attachments.tool_registry_attached;
        inspection.attached_skill_loader = attachments.skill_loader_attached;
        inspection.attached_config_save_path = attachments.config_save_path_attached;
        if (runtime != nullptr) {
            inspection.tool_definitions = runtime->tools().definitions();
        }
        inspection.active_skill_names = collect_active_skill_names(skill_loader);
        inspection.runtime_build_error = std::string(runtime_build_error);
        return inspection;
    }

    bool maybe_skip_web_server_start_for_tests(storage::SessionStore *session_store, memory::MemoryStore *memory_store, const AgentRuntimeBundle *runtime,
                                               skills::SkillLoader *skill_loader, const WebServerRuntimeAttachments &attachments, std::string_view runtime_build_error) {
        return detail::inspect_web_startup_for_tests(build_web_startup_inspection(session_store, memory_store, runtime, skill_loader, attachments, runtime_build_error));
    }

    WebServerRuntimeAttachments configure_web_server_runtime(web::WebServer &web_server, const CliOptions &options, config::Config &cfg, storage::SessionStore *session_store,
                                                             memory::MemoryStore *memory_store, automation::Runtime *automation_runtime, tools::ToolRegistry *tool_registry,
                                                             skills::SkillLoader *skill_loader) {
        WebServerRuntimeAttachments attachments;
        web_server.set_static_dir(options.web_dir);
        web_server.set_config(&cfg);
        const auto config_save_path = config::default_orangutan_config_path();
        if (!config_save_path.empty()) {
            web_server.set_config_save_path(config_save_path);
            attachments.config_save_path_attached = true;
        }
        if (session_store != nullptr) {
            web_server.set_session_store(session_store);
            attachments.session_store_attached = true;
        }
        web_server.set_memory_store(memory_store);
        web_server.set_automation_runtime(automation_runtime);
        if (tool_registry != nullptr) {
            web_server.set_tool_registry(tool_registry);
            attachments.tool_registry_attached = true;
        }
        if (skill_loader != nullptr) {
            web_server.set_skill_loader(skill_loader);
            attachments.skill_loader_attached = true;
        }
        return attachments;
    }

    void warn_if_nonlocal_web_host(const std::string &web_host) {
        if (web_host != "127.0.0.1") {
            spdlog::warn("Web server binding to {} — accessible from network", web_host);
        }
    }

    void deactivate_runtime_completion_resume_state(const std::shared_ptr<RuntimeCompletionResumeState> &state) {
        if (state == nullptr) {
            return;
        }

        std::scoped_lock lock(state->mutex);
        state->agent = nullptr;
        state->provider = nullptr;
        state->hook_manager = nullptr;
        state->session_store = nullptr;
        state->current_session_id = nullptr;
        state->automation_runtime = nullptr;
    }

    RuntimeCompletionResumeStateGuard::RuntimeCompletionResumeStateGuard(std::shared_ptr<RuntimeCompletionResumeState> state_ptr)
    : state_(std::move(state_ptr)) {}

    RuntimeCompletionResumeStateGuard::~RuntimeCompletionResumeStateGuard() {
        deactivate_runtime_completion_resume_state(state_);
    }

    BackgroundCompletionResumeCallback make_runtime_completion_resume_callback(const std::weak_ptr<RuntimeCompletionResumeState> &weak_state) {
        return [weak_state](const std::string &message) -> std::optional<std::string> {
            const auto state = weak_state.lock();
            if (!state) {
                return "runtime is no longer available";
            }

            std::scoped_lock lock(state->mutex);
            if (state->agent == nullptr) {
                return "runtime is no longer available";
            }

            return cli::run_completion_resume_message(
                *state->agent, message, state->agent_key, state->automation_runtime,
                [state](const std::string &) -> std::optional<std::string> {
                    if (!state->persist_session || state->session_store == nullptr || state->current_session_id == nullptr || state->agent == nullptr) {
                        return std::nullopt;
                    }

                    const bool created_session = state->current_session_id->empty();
                    const auto active_model = state->provider != nullptr && !state->provider->current_model().empty() ? state->provider->current_model() : state->configured_model;
                    if (!cli::persist_session(*state->agent, *state->session_store, *state->current_session_id,
                                              cli::make_cli_session_metadata(active_model, state->scope_key, state->agent_key))) {
                        return std::nullopt;
                    }

                    if (created_session) {
                        dispatch_session_start(state->hook_manager, *state->current_session_id, state->agent->history().size());
                    }
                    return std::nullopt;
                },
                state->suppress_human_output);
        };
    }

    std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_runtime_background_completion_bindings(automation::Runtime *automation_runtime,
                                                                                                           BackgroundCompletionResumeCallback resume_callback) {
        if (automation_runtime == nullptr) {
            return nullptr;
        }

        return make_background_completion_runtime_bindings(
            [automation_runtime](const automation::InboxItem &item) {
                static_cast<void>(automation_runtime->store().insert_inbox(item));
            },
            std::move(resume_callback));
    }

} // namespace orangutan::bootstrap
