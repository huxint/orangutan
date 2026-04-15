#include "bootstrap/app-runtime.hpp"

namespace orangutan::bootstrap {

    AppRuntime::AppRuntime()
    : automation_repository_(std::make_unique<orangutan::automation::Repository>()),
      automation_service_(std::make_unique<orangutan::automation::AutomationService>(*automation_repository_)),
      automation_runtime_(std::make_unique<orangutan::automation::AutomationRuntime>(*automation_service_)) {}

    AppRuntime::AppRuntime(const std::filesystem::path &automation_db_path)
    : automation_repository_(std::make_unique<orangutan::automation::Repository>(automation_db_path)),
      automation_service_(std::make_unique<orangutan::automation::AutomationService>(*automation_repository_)),
      automation_runtime_(std::make_unique<orangutan::automation::AutomationRuntime>(*automation_service_)) {}

    orangutan::automation::Repository &AppRuntime::automation_repository() noexcept {
        return *automation_repository_;
    }

    const orangutan::automation::Repository &AppRuntime::automation_repository() const noexcept {
        return *automation_repository_;
    }

    orangutan::automation::AutomationService &AppRuntime::automation_service() noexcept {
        return *automation_service_;
    }

    const orangutan::automation::AutomationService &AppRuntime::automation_service() const noexcept {
        return *automation_service_;
    }

    orangutan::automation::AutomationRuntime &AppRuntime::automation_runtime() noexcept {
        return *automation_runtime_;
    }

    const orangutan::automation::AutomationRuntime &AppRuntime::automation_runtime() const noexcept {
        return *automation_runtime_;
    }

} // namespace orangutan::bootstrap
