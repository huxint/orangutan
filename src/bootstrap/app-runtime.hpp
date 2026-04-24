#pragma once

#include "automation/repository.hpp"
#include "automation/runtime.hpp"
#include "automation/service.hpp"
#include "utils/task-pool.hpp"

#include <filesystem>
#include <memory>

namespace orangutan::bootstrap {

    class AppRuntime {
    public:
        AppRuntime();
        explicit AppRuntime(const std::filesystem::path &automation_db_path);
        ~AppRuntime();

        AppRuntime(const AppRuntime &) = delete;
        AppRuntime &operator=(const AppRuntime &) = delete;
        AppRuntime(AppRuntime &&) = delete;
        AppRuntime &operator=(AppRuntime &&) = delete;

        void start();
        void stop();

        orangutan::automation::Repository &automation_repository() noexcept;
        const orangutan::automation::Repository &automation_repository() const noexcept;
        orangutan::automation::AutomationService &automation_service() noexcept;
        const orangutan::automation::AutomationService &automation_service() const noexcept;
        orangutan::automation::AutomationRuntime &automation_runtime() noexcept;
        const orangutan::automation::AutomationRuntime &automation_runtime() const noexcept;

        [[nodiscard]]
        utils::TaskPool &task_pool() noexcept;

        [[nodiscard]]
        const utils::TaskPool &task_pool() const noexcept;

    private:
        std::unique_ptr<utils::TaskPool> task_pool_;
        std::unique_ptr<orangutan::automation::Repository> automation_repository_;
        std::unique_ptr<orangutan::automation::AutomationService> automation_service_;
        std::unique_ptr<orangutan::automation::AutomationRuntime> automation_runtime_;
    };

} // namespace orangutan::bootstrap
