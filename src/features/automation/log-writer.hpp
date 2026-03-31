#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace orangutan::automation {

    class LogWriter {
    public:
        [[nodiscard]]
        static std::string append(const std::string &workspace_root, const nlohmann::json &entry);
    };

} // namespace orangutan::automation
