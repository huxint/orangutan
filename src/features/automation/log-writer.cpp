#include "features/automation/log-writer.hpp"
#include "infra/files/file.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <print>
#include <stdexcept>

namespace orangutan::automation {

std::string LogWriter::append(const std::string &workspace_root, const json &entry) {
    if (workspace_root.empty()) {
        throw std::runtime_error("workspace root is empty");
    }

    const auto now = std::chrono::system_clock::now();
    const auto time_value = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time_value, &local_tm);

    std::array<char, 16> file_name{};
    std::strftime(file_name.data(), file_name.size(), "%Y-%m-%d", &local_tm);

    const auto log_dir = std::filesystem::path(workspace_root) / "automation" / "logs";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        throw std::runtime_error("failed to create automation log directory: " + ec.message());
    }

    const auto log_path = log_dir / (std::string(file_name.data()) + ".jsonl");
    fileio::File file = [&]() -> fileio::File {
        try {
            return fileio::File(log_path, "a");
        } catch (const std::runtime_error &) {
            throw std::runtime_error("failed to open automation log file: " + log_path.string());
        }
    }();

    try {
        std::println(file.get(), "{}", entry.dump());
        file.close();
    } catch (const std::exception &) {
        throw std::runtime_error("failed to write automation log file: " + log_path.string());
    }

    return log_path.string();
}

} // namespace orangutan::automation
