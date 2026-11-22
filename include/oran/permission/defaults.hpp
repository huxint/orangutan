#pragma once

#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

/// Construct the mode's baseline rules without reading configuration or state.
[[nodiscard]] RuleSet default_rules(Mode mode);

}  // namespace orangutan::permission
