#include "app/runtime/app-runtime.hpp"

namespace orangutan::app {

AppRuntime::AppRuntime()
: automation_store_(std::make_unique<orangutan::automation::Store>()),
  automation_runtime_(std::make_unique<orangutan::automation::Runtime>(*automation_store_)) {}

AppRuntime::AppRuntime(const std::filesystem::path &automation_db_path)
: automation_store_(std::make_unique<orangutan::automation::Store>(automation_db_path)),
  automation_runtime_(std::make_unique<orangutan::automation::Runtime>(*automation_store_)) {}

orangutan::automation::Store &AppRuntime::automation_store() noexcept {
    return *automation_store_;
}

const orangutan::automation::Store &AppRuntime::automation_store() const noexcept {
    return *automation_store_;
}

orangutan::automation::Runtime &AppRuntime::automation_runtime() noexcept {
    return *automation_runtime_;
}

const orangutan::automation::Runtime &AppRuntime::automation_runtime() const noexcept {
    return *automation_runtime_;
}

} // namespace orangutan::app
