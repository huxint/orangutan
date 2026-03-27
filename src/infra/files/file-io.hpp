#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::fileio {

    // Reads entire file into a string (binary-safe).
    // Throws std::runtime_error on failure.
    [[nodiscard]]
    std::string read_file(const std::filesystem::path &path);

    // Reads entire file into a string (binary-safe).
    // Returns std::nullopt on any failure — useful when missing files are expected.
    [[nodiscard]]
    std::optional<std::string> try_read_file(const std::filesystem::path &path);

    // Writes content to a text file (mode "w"), creating or truncating.
    // Throws std::runtime_error on failure.
    void write_file(const std::filesystem::path &path, std::string_view content);

    // Writes content to a binary file (mode "wb"), creating or truncating.
    // Throws std::runtime_error on failure.
    void write_file_binary(const std::filesystem::path &path, std::string_view content);

} // namespace orangutan::fileio
