#pragma once

#include "automation/model.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace orangutan::automation {

    class LogWriter {
    public:
        [[nodiscard]]
        static std::string append(std::string_view workspace_root, const nlohmann::json &entry);

        [[nodiscard]]
        static std::string append_run(std::string_view workspace_root, const Automation &automation, const RunRecord &run);
    };

} // namespace orangutan::automation
