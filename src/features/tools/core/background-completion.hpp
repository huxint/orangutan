#pragma once

#include "core/tools/tool.hpp"
#include "infra/subprocess/subprocess.hpp"

#include <string_view>

namespace orangutan {

inline constexpr std::string_view background_completion_mode_metadata_key = "on_complete.mode";
inline constexpr std::string_view background_completion_prompt_metadata_key = "on_complete.prompt";

class BackgroundCompletionDispatcher {
public:
    explicit BackgroundCompletionDispatcher(const ToolRuntimeContext *tool_context);

    [[nodiscard]]
    bool supports_completion_routing() const;

    [[nodiscard]]
    bool supports_resume_callback() const;

    void dispatch(const BackgroundProcessCompletionEvent &event) const;

private:
    std::string runtime_key_;
    std::string agent_key_;
    std::weak_ptr<BackgroundCompletionRuntimeBindings> background_completion_runtime_;
};

} // namespace orangutan
