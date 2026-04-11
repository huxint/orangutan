#include "utils/file-io.hpp"
#include "utils/file.hpp"

#include <cstdio>
#include <stdexcept>

namespace orangutan::fileio {

    namespace {

        void write_file_impl(const std::filesystem::path &path, std::string_view content, std::string_view mode) {
            File file(path, mode);
            if (!content.empty()) {
                const auto written = std::fwrite(content.data(), sizeof(char), content.size(), file.get());
                if (written != content.size()) {
                    throw std::runtime_error("failed to write file: " + path.string());
                }
            }
            file.close();
        }

    } // namespace

    std::string read_file(const std::filesystem::path &path) {
        std::error_code ec;
        const auto size_hint = std::filesystem::file_size(path, ec);

        File file(path, "rb");
        std::string content;
        if (ec == std::error_code{}) {
            content.reserve(static_cast<std::size_t>(size_hint));
        }

        constexpr std::size_t READ_CHUNK_SIZE = 16384;
        std::string chunk;

        while (true) {
            chunk.resize_and_overwrite(READ_CHUNK_SIZE, [&file](char *buffer, std::size_t size) {
                return std::fread(buffer, sizeof(char), size, file.get());
            });

            if (chunk.empty()) {
                if (std::ferror(file.get()) != 0) {
                    throw std::runtime_error("failed to read file: " + path.string());
                }
                break;
            }

            content.append(chunk);
        }

        if (std::ferror(file.get()) != 0) {
            throw std::runtime_error("failed to read file: " + path.string());
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
        write_file_impl(path, content, "w");
    }

    void write_file_binary(const std::filesystem::path &path, std::string_view content) {
        write_file_impl(path, content, "wb");
    }

} // namespace orangutan::fileio
