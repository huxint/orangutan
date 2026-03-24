#pragma once

#include "features/automation/runtime.hpp"
#include "features/automation/store.hpp"

#include <filesystem>
#include <memory>

namespace orangutan::app {

class AppRuntime {
public:
    AppRuntime();
    explicit AppRuntime(const std::filesystem::path &automation_db_path);
    ~AppRuntime() = default;

    AppRuntime(const AppRuntime &) = delete;
    AppRuntime &operator=(const AppRuntime &) = delete;
    AppRuntime(AppRuntime &&) = delete;
    AppRuntime &operator=(AppRuntime &&) = delete;

    orangutan::automation::Store &automation_store() noexcept;
    const orangutan::automation::Store &automation_store() const noexcept;
    orangutan::automation::Runtime &automation_runtime() noexcept;
    const orangutan::automation::Runtime &automation_runtime() const noexcept;

private:
    std::unique_ptr<orangutan::automation::Store> automation_store_;
    std::unique_ptr<orangutan::automation::Runtime> automation_runtime_;
};

} // namespace orangutan::app
