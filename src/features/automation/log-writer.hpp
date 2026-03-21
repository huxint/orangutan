#pragma once

#include "core/types.hpp"

#include <string>

namespace orangutan::automation {

class LogWriter {
public:
    [[nodiscard]]
    std::string append(const std::string &workspace_root, const json &entry) const;
};

} // namespace orangutan::automation
