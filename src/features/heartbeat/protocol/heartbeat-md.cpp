#include "features/heartbeat/protocol/heartbeat-md.hpp"

#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>

namespace orangutan {
namespace {

constexpr std::size_t heartbeat_md_size_warning = 10240;

bool is_empty_or_headers_only(const std::string &content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        auto trimmed = std::string_view(line).substr(start);
        if (trimmed.starts_with('#')) {
            auto after_hashes = trimmed.find_first_not_of('#');
            if (after_hashes == std::string_view::npos) {
                continue;
            }
            auto rest = trimmed.substr(after_hashes);
            if (rest.find_first_not_of(" \t\r\n") == std::string_view::npos) {
                continue;
            }
        }
        return false;
    }
    return true;
}

} // namespace

std::optional<std::string> load_heartbeat_md(const std::string &path) {
    if (path.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    const auto heartbeat_path = std::filesystem::path(path);
    if (heartbeat_path.extension() != ".md") {
        return std::nullopt;
    }

    if (!std::filesystem::exists(heartbeat_path, ec)) {
        return std::nullopt;
    }

    auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        spdlog::warn("Failed to get size of HEARTBEAT.md at '{}': {}", path, ec.message());
        return std::nullopt;
    }

    if (file_size > heartbeat_md_size_warning) {
        spdlog::warn("HEARTBEAT.md at '{}' is {}KB — large files waste tokens on every heartbeat run", path, file_size / 1024);
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::warn("Failed to open HEARTBEAT.md at '{}'", path);
        return std::nullopt;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (is_empty_or_headers_only(content)) {
        return std::string{};
    }

    return content;
}

} // namespace orangutan
