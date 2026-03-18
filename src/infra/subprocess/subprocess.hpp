#pragma once

#include <chrono>
#include <string>

namespace orangutan {

struct SubprocessConfig {
    std::string command;
    std::string stdin_data;
    std::chrono::seconds timeout{30};
    std::string working_dir;
    bool use_shell = true;
};

struct SubprocessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out = false;
};

// Run a subprocess with poll-based I/O multiplexing, deadline timeout, and process group cleanup.
SubprocessResult run_subprocess(const SubprocessConfig &config);

} // namespace orangutan
