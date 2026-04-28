#pragma once

#include "process/subprocess.hpp"
#include "tools/registry/tool-context.hpp"

#include <cstddef>
#include <string_view>

namespace orangutan::tools {

    inline constexpr std::string_view BACKGROUND_COMPLETION_MODE_METADATA_KEY = "on_complete.mode";
    inline constexpr std::string_view BACKGROUND_COMPLETION_PROMPT_METADATA_KEY = "on_complete.prompt";
    inline constexpr std::size_t BACKGROUND_COMPLETION_PROMPT_MAX_CHARS = 2048;
    inline constexpr std::size_t BACKGROUND_COMPLETION_PAYLOAD_MAX_BYTES = 16384;

    class BackgroundCompletionDispatcher {
    public:
        explicit BackgroundCompletionDispatcher(BackgroundCompletionCapability capability);
        explicit BackgroundCompletionDispatcher(const ToolRuntimeContext *tool_context);

        [[nodiscard]]
        bool supports_completion_routing() const;

        [[nodiscard]]
        bool supports_resume_callback() const;

        void dispatch(const BackgroundProcessCompletionEvent &event) const;

    private:
        std::string runtime_key_;
        std::string agent_key_;
        bool supports_completion_routing_ = false;
        bool supports_resume_callback_ = false;
        std::shared_ptr<const BackgroundCompletionRuntimeBindings> background_completion_runtime_;
    };

} // namespace orangutan::tools
