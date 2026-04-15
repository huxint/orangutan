#pragma once

#include "channel/channel.hpp"
#include "permissions/permission-types.hpp"
#include "types/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::automation {
    class AutomationService;
    class AutomationRuntime;
    struct DeliveryRecord;
} // namespace orangutan::automation

namespace orangutan::coordinator {
    class CoordinatorManager;
}

namespace orangutan::swarm {
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::swarm

namespace orangutan::tools {
    using BackgroundCompletionResumeCallback = std::function<std::optional<std::string>(const std::string &message)>;
    using BackgroundCompletionDeliveryCallback = std::function<void(const automation::DeliveryRecord &delivery)>;
    using AttachmentDownloadCallback = std::function<Attachment(const Attachment &attachment, const std::string &destination_path)>;
    using PermissionRuleMutationCallback = std::function<void(PermissionRule rule)>;
    using RuntimeAbortChecker = std::function<bool()>;

    class BackgroundCompletionRuntimeBindings {
    public:
        BackgroundCompletionRuntimeBindings(BackgroundCompletionDeliveryCallback delivery_callback, BackgroundCompletionResumeCallback resume_callback = {})
        : delivery_callback_(std::move(delivery_callback)),
          resume_callback_(std::move(resume_callback)) {}

        [[nodiscard]]
        bool supports_completion_routing() const {
            return delivery_callback_ != nullptr;
        }

        [[nodiscard]]
        bool supports_resume_callback() const {
            return resume_callback_ != nullptr;
        }

        [[nodiscard]]
        const BackgroundCompletionDeliveryCallback &delivery_callback() const {
            return delivery_callback_;
        }

        [[nodiscard]]
        const BackgroundCompletionResumeCallback &resume_callback() const {
            return resume_callback_;
        }

    private:
        BackgroundCompletionDeliveryCallback delivery_callback_;
        BackgroundCompletionResumeCallback resume_callback_;
    };

    [[nodiscard]]
    inline std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_background_completion_runtime_bindings(BackgroundCompletionDeliveryCallback delivery_callback,
                                                                                                                  BackgroundCompletionResumeCallback resume_callback = {}) {
        if (delivery_callback == nullptr) {
            return nullptr;
        }

        return std::make_shared<BackgroundCompletionRuntimeBindings>(std::move(delivery_callback), std::move(resume_callback));
    }

    struct ToolRuntimeContext {
        std::string runtime_key;
        std::string agent_key;
        std::string agent_name;
        std::string scope_key;
        std::string team_id;
        std::string *current_session_id = nullptr;
        coordinator::CoordinatorManager *coordinator_manager = nullptr;
        swarm::TeamManager *team_manager = nullptr;
        swarm::AgentMailbox *mailbox = nullptr;
        std::vector<std::string> team_agents;
        bool is_child_run = false;
        bool coordinator_mode = false;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id;
        automation::AutomationService *automation_service = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
        RuntimeAbortChecker abort_checker;
        ApprovalCallback approval_callback;
        PermissionRuleMutationCallback permission_rule_mutator;
        ToolPermissionContext *permission_context = nullptr;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
        std::vector<Attachment> current_message_attachments;
        AttachmentDownloadCallback attachment_download_callback;
    };

} // namespace orangutan::tools

namespace orangutan {

    using tools::AttachmentDownloadCallback;
    using tools::BackgroundCompletionDeliveryCallback;
    using tools::BackgroundCompletionResumeCallback;
    using tools::BackgroundCompletionRuntimeBindings;
    using tools::make_background_completion_runtime_bindings;
    using tools::PermissionRuleMutationCallback;
    using tools::RuntimeAbortChecker;
    using tools::ToolRuntimeContext;

} // namespace orangutan
