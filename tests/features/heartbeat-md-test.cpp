#include "features/heartbeat/protocol/heartbeat-md.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include "support/ut.hpp"
#include <string_view>

namespace orangutan {
namespace {

std::filesystem::path make_test_path(std::string_view filename) {
    return orangutan::testing::unique_test_path("heartbeat-md", filename);
}

bool write_test_file(const std::filesystem::path &path, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    file << content;
    return file.good();
}

boost::ut::suite heartbeat_md_suite = [] {
    using namespace boost::ut;

    "missing_file_returns_nullopt"_test = [] {
        const auto result = load_heartbeat_md("/nonexistent/path/HEARTBEAT.md");
        expect(not result.has_value());
    };

    "empty_path_returns_nullopt"_test = [] {
        const auto result = load_heartbeat_md("");
        expect(not result.has_value());
    };

    "loads_markdown_file_contents"_test = [] {
        const auto path = make_test_path("HEARTBEAT-load.md");
        expect(write_test_file(path, "# heartbeat\nready\n") >> fatal) << "expected heartbeat fixture file to be written";

        const auto result = load_heartbeat_md(path.string());

        expect(result.has_value() >> fatal) << "expected heartbeat markdown to load";
        expect(*result == "# heartbeat\nready\n");
    };

    "ignores_non_markdown_extension"_test = [] {
        const auto path = make_test_path("HEARTBEAT.txt");
        expect(write_test_file(path, "not markdown") >> fatal) << "expected non-markdown fixture file to be written";

        const auto result = load_heartbeat_md(path.string());

        expect(not result.has_value());
    };

    "returns_nullopt_when_file_cannot_be_opened"_test = [] {
        const auto path = make_test_path("missing/HEARTBEAT.md");
        std::filesystem::remove_all(path.parent_path());

        const auto result = load_heartbeat_md(path.string());

        expect(not result.has_value());
    };
};

} // namespace
} // namespace orangutan
