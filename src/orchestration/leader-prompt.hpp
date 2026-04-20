#pragma once

#include <string>
#include <vector>

namespace orangutan::orchestration {

    [[nodiscard]]
    std::string get_leader_system_prompt(const std::vector<std::string> &available_agents);

    [[nodiscard]]
    std::string get_worker_system_prompt_addendum(const std::string &agent_key, const std::string &task_description);

} // namespace orangutan::orchestration
