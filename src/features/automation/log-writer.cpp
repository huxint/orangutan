#include "features/automation/log-writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace orangutan::automation {

std::string LogWriter::append(const std::string &workspace_root, const json &entry) const {
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
    std::ofstream out(log_path, std::ios::app);
    if (!out.is_open()) {
        throw std::runtime_error("failed to open automation log file: " + log_path.string());
    }
    std::println(out, "{}", entry.dump());
    return log_path.string();
}

} // namespace orangutan::automation
