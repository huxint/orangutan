#pragma once

#include "core/types.hpp"

#include <vector>

namespace orangutan::app {

    [[nodiscard]]
    json make_session_event_json(const char *type, const std::string &session_id, const std::string &action);
    [[nodiscard]]
    json build_edit_details(const ToolUseBlock &call);
    [[nodiscard]]
    std::vector<json> build_session_history_events(const std::vector<Message> &history);

} // namespace orangutan::app
