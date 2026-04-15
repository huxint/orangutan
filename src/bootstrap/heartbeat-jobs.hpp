#pragma once

#include "config/config.hpp"

namespace orangutan::automation {
    class AutomationService;
}

namespace orangutan::bootstrap {

    void reconcile_heartbeat_jobs(const Config &cfg, automation::AutomationService &service);

} // namespace orangutan::bootstrap
