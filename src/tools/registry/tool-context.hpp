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
    class Runtime;
    struct InboxItem;
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
    using BackgroundCompletionInboxCallback = std::function<void(const automation::InboxItem &item)>;
    using AttachmentDownloadCallback = std::function<Attachment(const Attachment &attachment, const std::string &destination_path)>;
    using PermissionRuleMutationCallback = std::function<void(PermissionRule rule)>;
    using RuntimeAbortChecker = std::function<bool()>;

    class BackgroundCompletionRuntimeBindings {
    public:
        BackgroundCompletionRuntimeBindings(BackgroundCompletionInboxCallback inbox_callback, BackgroundCompletionResumeCallback resume_callback = {})
        : inbox_callback_(std::move(inbox_callback)),
          resume_callback_(std::move(resume_callback)) {}

        [[nodiscard]]
        bool supports_completion_routing() const {
            return inbox_callback_ != nullptr;
        }

        [[nodiscard]]
        bool supports_resume_callback() const {
            return resume_callback_ != nullptr;
        }

        [[nodiscard]]
        const BackgroundCompletionInboxCallback &inbox_callback() const {
            return inbox_callback_;
        }

        [[nodiscard]]
        const BackgroundCompletionResumeCallback &resume_callback() const {
            return resume_callback_;
        }

    private:
        BackgroundCompletionInboxCallback inbox_callback_;
        BackgroundCompletionResumeCallback resume_callback_;
    };

    [[nodiscard]]
    inline std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_background_completion_runtime_bindings(BackgroundCompletionInboxCallback inbox_callback,
                                                                                                                  BackgroundCompletionResumeCallback resume_callback = {}) {
        if (inbox_callback == nullptr) {
            return nullptr;
        }

        return std::make_shared<BackgroundCompletionRuntimeBindings>(std::move(inbox_callback), std::move(resume_callback));
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
        automation::Runtime *automation_runtime = nullptr;
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
    using tools::BackgroundCompletionInboxCallback;
    using tools::BackgroundCompletionResumeCallback;
    using tools::BackgroundCompletionRuntimeBindings;
    using tools::make_background_completion_runtime_bindings;
    using tools::PermissionRuleMutationCallback;
    using tools::RuntimeAbortChecker;
    using tools::ToolRuntimeContext;

} // namespace orangutan
