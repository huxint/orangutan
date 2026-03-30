#include "features/heartbeat/protocol/heartbeat-md.hpp"
#include "infra/files/file-io.hpp"

#include <filesystem>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>

namespace orangutan {
    namespace {

        constexpr std::std::size_t heartbeat_md_size_warning = 10240;

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

    std::optional<std::string> load_heartbeat_md(const std::filesystem::path &path) {
        if (path.empty()) {
            return std::nullopt;
        }

        if (path.extension() != ".md") {
            return std::nullopt;
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return std::nullopt;
        }

        const auto file_size = std::filesystem::file_size(path, ec);
        if (ec) {
            spdlog::warn("Failed to get size of HEARTBEAT.md at '{}': {}", path.string(), ec.message());
            return std::nullopt;
        }

        if (file_size > heartbeat_md_size_warning) {
            spdlog::warn("HEARTBEAT.md at '{}' is {}KB — large files waste tokens on every heartbeat run", path.string(), file_size / 1024);
        }

        auto content = fileio::try_read_file(path);
        if (!content.has_value()) {
            spdlog::warn("Failed to open HEARTBEAT.md at '{}'", path.string());
            return std::nullopt;
        }
        if (is_empty_or_headers_only(*content)) {
            return std::string{};
        }

        return content;
    }

} // namespace orangutan
