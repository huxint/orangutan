#pragma once

#include <string>
#include <vector>

namespace orangutan::coordinator {

    [[nodiscard]]
    std::string get_coordinator_system_prompt(const std::vector<std::string> &available_agents);

    [[nodiscard]]
    std::vector<std::string> get_coordinator_allowed_tools();

    [[nodiscard]]
    std::string get_worker_system_prompt_addendum(const std::string &agent_key, const std::string &task_description);

} // namespace orangutan::coordinator
