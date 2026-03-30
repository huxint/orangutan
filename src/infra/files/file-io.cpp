#include "infra/files/file-io.hpp"
#include "infra/files/file.hpp"

#include <cstdio>
#include <spdlog/common.h>
#include <stdexcept>

namespace orangutan::fileio {

    std::string read_file(const std::filesystem::path &path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec != std::error_code{}) {
            throw std::runtime_error("failed to get file size: " + path.string());
        }

        File file(path, "rb");
        std::string content(static_cast<std::size_t>(size), '\0');
        if (!content.empty()) {
            const auto bytes_read = std::fread(content.data(), sizeof(char), content.size(), file.get());
            if (bytes_read != content.size()) {
                throw std::runtime_error("failed to read file: " + path.string());
            }
        }
        file.close();
        return content;
    }

    std::optional<std::string> try_read_file(const std::filesystem::path &path) {
        try {
            return read_file(path);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    void write_file(const std::filesystem::path &path, std::string_view content) {
        File file(path, "w");
        try {
            spdlog::fmt_lib::print(file.get(), "{}", content);
            file.close();
        } catch (const std::exception &) {
            throw std::runtime_error("failed to write file: " + path.string());
        }
    }

    void write_file_binary(const std::filesystem::path &path, std::string_view content) {
        File file(path, "wb");
        if (!content.empty()) {
            const auto bytes_written = std::fwrite(content.data(), sizeof(char), content.size(), file.get());
            if (bytes_written != content.size()) {
                throw std::runtime_error("failed to write file: " + path.string());
            }
        }
        file.close();
    }

} // namespace orangutan::fileio
