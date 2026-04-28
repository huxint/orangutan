#pragma once

#include "channel/channel.hpp"
#include "orchestration/types.hpp"
#include "permissions/permission-types.hpp"
#include "types/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::automation {
    class AutomationService;
    class AutomationRuntime;
    struct DeliveryRecord;
} // namespace orangutan::automation

namespace orangutan::orchestration {
    class OrchestrationManager;
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::orchestration

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

    struct RuntimeIdentityContext {
        std::string_view runtime_key;
        std::string_view agent_key;
        std::string_view agent_name;
        std::string_view scope_key;
        std::string_view team_id;
        orchestration::agent_role role = orchestration::agent_role::standalone;
        base::origin runtime_origin = base::origin::cli;
        std::string_view raw_caller_id;
    };

    struct OrchestrationCapability {
        orchestration::OrchestrationManager *orchestration_manager = nullptr;
        orchestration::TeamManager *team_manager = nullptr;
        orchestration::AgentMailbox *mailbox = nullptr;
    };

    struct AutomationCapability {
        std::string_view agent_key;
        automation::AutomationService *automation_service = nullptr;
        automation::AutomationRuntime *automation_runtime = nullptr;
    };

    struct AttachmentCapability {
        base::origin runtime_origin = base::origin::cli;
        const std::vector<Attachment> *current_message_attachments = nullptr;
        AttachmentDownloadCallback attachment_download_callback;
    };

    struct BackgroundCompletionCapability {
        std::string_view runtime_key;
        std::string_view agent_key;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime;
    };

    struct ToolRuntimeContext {
        std::string runtime_key;
        std::string agent_key;
        std::string agent_name;
        std::string scope_key;
        std::string team_id;
        std::string *current_session_id = nullptr;
        orchestration::OrchestrationManager *orchestration_manager = nullptr;
        orchestration::TeamManager *team_manager = nullptr;
        orchestration::AgentMailbox *mailbox = nullptr;
        orchestration::agent_role role = orchestration::agent_role::standalone;
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

    [[nodiscard]]
    inline RuntimeIdentityContext runtime_identity_context(const ToolRuntimeContext &context) {
        return RuntimeIdentityContext{
            .runtime_key = context.runtime_key,
            .agent_key = context.agent_key,
            .agent_name = context.agent_name,
            .scope_key = context.scope_key,
            .team_id = context.team_id,
            .role = context.role,
            .runtime_origin = context.runtime_origin,
            .raw_caller_id = context.raw_caller_id,
        };
    }

    [[nodiscard]]
    inline RuntimeIdentityContext runtime_identity_context(const ToolRuntimeContext *context) {
        return context != nullptr ? runtime_identity_context(*context) : RuntimeIdentityContext{};
    }

    [[nodiscard]]
    inline OrchestrationCapability orchestration_capability(const ToolRuntimeContext &context) {
        return OrchestrationCapability{
            .orchestration_manager = context.orchestration_manager,
            .team_manager = context.team_manager,
            .mailbox = context.mailbox,
        };
    }

    [[nodiscard]]
    inline OrchestrationCapability orchestration_capability(const ToolRuntimeContext *context) {
        return context != nullptr ? orchestration_capability(*context) : OrchestrationCapability{};
    }

    [[nodiscard]]
    inline AutomationCapability automation_capability(const ToolRuntimeContext &context) {
        return AutomationCapability{
            .agent_key = context.agent_key,
            .automation_service = context.automation_service,
            .automation_runtime = context.automation_runtime,
        };
    }

    [[nodiscard]]
    inline AutomationCapability automation_capability(const ToolRuntimeContext *context) {
        return context != nullptr ? automation_capability(*context) : AutomationCapability{};
    }

    [[nodiscard]]
    inline AttachmentCapability attachment_capability(const ToolRuntimeContext &context) {
        return AttachmentCapability{
            .runtime_origin = context.runtime_origin,
            .current_message_attachments = &context.current_message_attachments,
            .attachment_download_callback = context.attachment_download_callback,
        };
    }

    [[nodiscard]]
    inline AttachmentCapability attachment_capability(const ToolRuntimeContext *context) {
        return context != nullptr ? attachment_capability(*context) : AttachmentCapability{};
    }

    [[nodiscard]]
    inline BackgroundCompletionCapability background_completion_capability(const ToolRuntimeContext &context) {
        return BackgroundCompletionCapability{
            .runtime_key = context.runtime_key,
            .agent_key = context.agent_key,
            .background_completion_runtime = context.background_completion_runtime,
        };
    }

    [[nodiscard]]
    inline BackgroundCompletionCapability background_completion_capability(const ToolRuntimeContext *context) {
        return context != nullptr ? background_completion_capability(*context) : BackgroundCompletionCapability{};
    }

} // namespace orangutan::tools

namespace orangutan {

    using tools::AttachmentCapability;
    using tools::AttachmentDownloadCallback;
    using tools::AutomationCapability;
    using tools::BackgroundCompletionCapability;
    using tools::BackgroundCompletionDeliveryCallback;
    using tools::BackgroundCompletionResumeCallback;
    using tools::BackgroundCompletionRuntimeBindings;
    using tools::make_background_completion_runtime_bindings;
    using tools::OrchestrationCapability;
    using tools::PermissionRuleMutationCallback;
    using tools::RuntimeAbortChecker;
    using tools::RuntimeIdentityContext;
    using tools::ToolRuntimeContext;

} // namespace orangutan
