#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace orangutan::testing {

/// Returns a deterministic temporary root under <project>/tmp/tests, creating it if needed.
inline std::filesystem::path test_tmp_root() {
    const auto root = std::filesystem::current_path() / "tmp" / "tests";
    std::filesystem::create_directories(root);
    return root;
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

} // namespace orangutan::testing
