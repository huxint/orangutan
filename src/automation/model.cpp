#include "automation/model.hpp"

#include "automation/builder.hpp"

namespace orangutan::automation {

    AutomationBuilder Automation::named(std::string_view name) {
        return AutomationBuilder{name};
    }

} // namespace orangutan::automation
