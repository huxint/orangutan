#include "automation/log-writer.hpp"
#include "utils/file.hpp"
#include "utils/time-format.hpp"

#include <filesystem>
#include <spdlog/common.h>
#include <stdexcept>

namespace orangutan::automation {

    std::string LogWriter::append(std::string_view workspace_root, const nlohmann::json &entry) {
        if (workspace_root.empty()) {
            throw std::runtime_error("workspace root is empty");
        }

        const auto file_name = time::current_local_date();
        const auto log_dir = std::filesystem::path(workspace_root) / "automation" / "logs";
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        if (ec) {
            throw std::runtime_error("failed to create automation log directory: " + ec.message());
        }

        const auto log_path = log_dir / (file_name + ".jsonl");
        fileio::File file = [&]() -> fileio::File {
            try {
                return fileio::File(log_path, "a");
            } catch (const std::runtime_error &) {
                throw std::runtime_error("failed to open automation log file: " + log_path.string());
            }
        }();

        try {
            spdlog::fmt_lib::println(file.get(), "{}", entry.dump());
            file.close();
        } catch (const std::exception &) {
            throw std::runtime_error("failed to write automation log file: " + log_path.string());
        }

        return log_path.string();
    }

} // namespace orangutan::automation
