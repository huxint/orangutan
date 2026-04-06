#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace orangutan::automation {

    class LogWriter {
    public:
        [[nodiscard]]
        static std::string append(std::string_view workspace_root, const nlohmann::json &entry);
    };

} // namespace orangutan::automation
