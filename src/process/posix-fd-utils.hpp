#pragma once

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

#include "utils/format.hpp"

namespace orangutan::process {

    inline void write_all(int fd, std::string_view text) {
        std::size_t written = 0;
        while (written < text.size()) {
            const auto pending = text.substr(written);
            const auto count = ::write(fd, pending.data(), pending.size());
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    inline void write_child_error(std::string_view operation, std::string_view path) {
        std::string message(operation);
        utils::format_to(message, "({}) failed: {}\n", path, std::strerror(errno));
        write_all(STDERR_FILENO, message);
    }

    [[nodiscard]]
    inline int remaining_ms(std::chrono::steady_clock::time_point deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return 0;
        }
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }

    [[nodiscard]]
    inline bool set_nonblocking(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0); // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (flags == -1) {
            return false;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1; // NOLINT(cppcoreguidelines-pro-type-vararg)
    }

} // namespace orangutan::process
