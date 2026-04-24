#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::string json_dump_lossy(const nlohmann::json &value, int indent = -1) {
        return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
    }

} // namespace orangutan::utils
