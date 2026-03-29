#pragma once

#include "core/types.hpp"

#include <vector>

namespace orangutan::app {

    [[nodiscard]]
    nlohmann::json make_session_event_json(const char *type, const std::string &session_id, const std::string &action);
    [[nodiscard]]
    nlohmann::json build_edit_details(const ToolUse &call);
    [[nodiscard]]
    std::vector<nlohmann::json> build_session_history_events(const std::vector<Message> &history);

} // namespace orangutan::app
