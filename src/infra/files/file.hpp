#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orangutan::fileio {

class File {
public:
    explicit File(const std::filesystem::path &path, std::string_view mode)
    : path_(path),
      fp_(open(path, mode)) {
        if (fp_ == nullptr) {
            throw std::runtime_error("failed to open file: " + path_.string());
        }
    }

    [[nodiscard]]
    std::FILE *get() const noexcept {
        return fp_.get();
    }

    void close() {
        if (fp_ == nullptr) {
            return;
        }

        auto *raw = fp_.release();   // NOLINT(cppcoreguidelines-owning-memory)
        if (std::fclose(raw) != 0) { // NOLINT(cppcoreguidelines-owning-memory)
            throw std::runtime_error("failed to close file: " + path_.string());
        }
    }

private:
    // clang-format off
    using file_ptr = std::unique_ptr<std::FILE, decltype([](std::FILE *fp) noexcept {
        if (fp != nullptr) {
            static_cast<void>(std::fclose(fp)); // NOLINT(cppcoreguidelines-owning-memory)
        }
    })>;
    // clang-format on

    [[nodiscard]]
    static file_ptr open(const std::filesystem::path &path, std::string_view mode) {
#if defined(_WIN32)
        const std::wstring wide_mode(mode.begin(), mode.end());
        return file_ptr(_wfopen(path.c_str(), wide_mode.c_str()));
#else
        const std::string narrow_mode(mode);
        return file_ptr(std::fopen(path.c_str(), narrow_mode.c_str()));
#endif
    }

    std::filesystem::path path_;
    file_ptr fp_;
};

} // namespace orangutan::fileio
