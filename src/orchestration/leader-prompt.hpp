#pragma once

#include <string>
#include <vector>

namespace orangutan::orchestration {

    [[nodiscard]]
    std::string get_leader_system_prompt();

    [[nodiscard]]
    std::string get_teammate_system_prompt_addendum(const std::string &agent_name, const std::string &task_description);

} // namespace orangutan::orchestration
