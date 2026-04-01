#pragma once

#include "types/base.hpp"
#include "types/tool-def.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <spdlog/spdlog.h>

namespace orangutan::testing {

    /// Returns a deterministic temporary root under <project>/tmp/tests, creating it if needed.
    inline std::filesystem::path test_tmp_root() {
        const auto root = std::filesystem::current_path() / "tmp" / "tests";
        std::filesystem::create_directories(root);
        return root;
    }

    /// Returns a unique temporary root under <project>/tmp/tests/<prefix>-<token> and creates it.
    inline std::filesystem::path unique_test_root(std::string_view prefix) {
        static std::atomic<base::u64> sequence{0};
        const auto token = std::to_string(static_cast<base::u64>(std::chrono::steady_clock::now().time_since_epoch().count())) + "-" +
                           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        auto root = test_tmp_root() / (std::string(prefix) + "-" + token);
        std::filesystem::create_directories(root);
        return root;
    }

    /// Returns a unique test path rooted under a fresh per-test directory.
    inline std::filesystem::path unique_test_path(std::string_view prefix, std::string_view relative_path = {}) {
        auto root = unique_test_root(prefix);
        if (!relative_path.empty()) {
            return root / std::filesystem::path(relative_path);
        }
        return root;
    }

    /// Returns a unique sqlite/database path under a fresh per-test directory.
    inline std::filesystem::path unique_test_db_path(std::string_view prefix, std::string_view filename = "test.db") {
        return unique_test_path(prefix, filename);
    }

    inline bool has_tool_named(const std::vector<ToolDef> &definitions, std::string_view name) {
        return std::ranges::any_of(definitions, [name](const ToolDef &definition) {
            return definition.name == name;
        });
    }

    /// RAII guard that sets an environment variable and restores (or unsets) it on destruction.
    class ScopedEnvVar {
    public:
        ScopedEnvVar(const char *name, const std::string &value)
        : name_(name) {
            if (const auto *current = std::getenv(name); current != nullptr) {
                had_previous_ = true;
                previous_ = current;
            }
            setenv(name_.c_str(), value.c_str(), 1);
        }

        ~ScopedEnvVar() {
            if (had_previous_) {
                setenv(name_.c_str(), previous_.c_str(), 1);
            } else {
                unsetenv(name_.c_str());
            }
        }

        ScopedEnvVar(const ScopedEnvVar &) = delete;
        ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;
        ScopedEnvVar(ScopedEnvVar &&) = delete;
        ScopedEnvVar &operator=(ScopedEnvVar &&) = delete;

    private:
        std::string name_;
        std::string previous_;
        bool had_previous_ = false;
    };

    template <typename Sink>
    class ScopedDefaultLogger {
    public:
        ScopedDefaultLogger(std::string name, const std::shared_ptr<Sink> &sink)
        : logger_(std::make_shared<spdlog::logger>(std::move(name), sink)),
          previous_(spdlog::default_logger()),
          previous_level_(spdlog::get_level()) {
            logger_->set_pattern("%l %v");
            spdlog::set_default_logger(logger_);
            spdlog::set_level(spdlog::level::debug);
        }

        ~ScopedDefaultLogger() {
            spdlog::set_default_logger(previous_);
            spdlog::set_level(previous_level_);
        }

        ScopedDefaultLogger(const ScopedDefaultLogger &) = delete;
        ScopedDefaultLogger &operator=(const ScopedDefaultLogger &) = delete;
        ScopedDefaultLogger(ScopedDefaultLogger &&) = delete;
        ScopedDefaultLogger &operator=(ScopedDefaultLogger &&) = delete;

    private:
        std::shared_ptr<spdlog::logger> logger_;
        std::shared_ptr<spdlog::logger> previous_;
        spdlog::level::level_enum previous_level_;
    };

} // namespace orangutan::testing
