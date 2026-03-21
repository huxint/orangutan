#pragma once

#include "core/tools/permissions.hpp"
#include "core/types.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orangutan {

namespace automation {
class Runtime;
}

class RuntimeMemory;
class SubagentManager;
using BackgroundCompletionResumeCallback = std::function<std::optional<std::string>(const std::string &message)>;
using BackgroundCompletionOwnerToken = std::shared_ptr<void>;

struct BackgroundCompletionRuntimeBindings {
    struct Snapshot {
        BackgroundCompletionOwnerToken owner_guard;
        automation::Runtime *automation_runtime = nullptr;
        BackgroundCompletionResumeCallback resume_callback;
    };

    BackgroundCompletionRuntimeBindings() = default;

    BackgroundCompletionRuntimeBindings(BackgroundCompletionOwnerToken owner_token, automation::Runtime *automation_runtime,
                                        BackgroundCompletionResumeCallback resume_callback = {})
    : owner_token_(std::move(owner_token)),
      automation_runtime_(automation_runtime),
      resume_callback_(std::move(resume_callback)) {}

    [[nodiscard]]
    Snapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        auto owner_guard = owner_token_.lock();
        if (owner_guard == nullptr) {
            return {};
        }

        return {
            .owner_guard = std::move(owner_guard),
            .automation_runtime = automation_runtime_,
            .resume_callback = resume_callback_,
        };
    }

    void invalidate() {
        std::scoped_lock lock(mutex_);
        owner_token_.reset();
        automation_runtime_ = nullptr;
        resume_callback_ = {};
    }

private:
    mutable std::mutex mutex_;
    std::weak_ptr<void> owner_token_;
    automation::Runtime *automation_runtime_ = nullptr;
    BackgroundCompletionResumeCallback resume_callback_;
};

[[nodiscard]]
inline std::shared_ptr<BackgroundCompletionRuntimeBindings> make_background_completion_runtime_bindings(BackgroundCompletionOwnerToken owner_token,
                                                                                                        automation::Runtime *automation_runtime,
                                                                                                        BackgroundCompletionResumeCallback resume_callback = {}) {
    if (owner_token == nullptr || automation_runtime == nullptr) {
        return nullptr;
    }

    return std::make_shared<BackgroundCompletionRuntimeBindings>(std::move(owner_token), automation_runtime, std::move(resume_callback));
}

struct ToolRuntimeContext {
    std::string runtime_key;
    std::string agent_key;
    std::string scope_key;
    std::string *current_session_id = nullptr;
    std::vector<std::string> allowed_child_agents;
    bool is_child_run = false;
    SubagentManager *subagent_manager = nullptr;
    SubagentRuntimeOrigin runtime_origin = SubagentRuntimeOrigin::cli;
    std::string raw_caller_id;
    automation::Runtime *automation_runtime = nullptr;
    ToolApprovalCallback approval_callback;
    std::shared_ptr<BackgroundCompletionRuntimeBindings> background_completion_runtime;

    void invalidate_background_completion_runtime() {
        automation_runtime = nullptr;
        if (background_completion_runtime == nullptr) {
            return;
        }

        background_completion_runtime->invalidate();
        background_completion_runtime.reset();
    }
};

struct Tool {
    ToolDef definition;
    std::function<std::string(const json &input)> execute;
};

class ToolRegistry {
public:
    using ExecutionGuard = std::function<std::optional<ToolResultBlock>(const ToolUseBlock &call)>;
    using DefinitionFilter = std::function<bool(const ToolDef &definition)>;

    void register_tool(Tool tool);
    void set_execution_guard(ExecutionGuard guard);
    void set_definition_filter(DefinitionFilter filter);

    std::vector<ToolDef> definitions() const;

    ToolResultBlock execute(const ToolUseBlock &call) const;

private:
    std::unordered_map<std::string, Tool> tools_;
    ExecutionGuard execution_guard_;
    DefinitionFilter definition_filter_;
};

[[nodiscard]]
std::string scrub_tool_output(std::string_view text);

void register_builtin_tools(ToolRegistry &registry, RuntimeMemory *runtime_memory = nullptr, const std::string &workspace = {}, const ToolRuntimeContext *tool_context = nullptr,
                            const ToolPermissionSettings *permissions = nullptr, std::string_view edit_mode = "search_replace");

} // namespace orangutan
