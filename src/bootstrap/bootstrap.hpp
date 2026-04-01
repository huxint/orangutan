#pragma once

#include "bootstrap/channel-serve.hpp"
#include "bootstrap/config-builder.hpp"
#include "subagent/subagent-manager.hpp"
#include "config/config.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace orangutan::bootstrap {

    int run(int argc, char **argv);

} // namespace orangutan::bootstrap
