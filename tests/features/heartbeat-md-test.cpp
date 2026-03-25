#include "features/heartbeat/protocol/heartbeat-md.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace orangutan {
namespace {

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

TEST_CASE("missing_file_returns_nullopt") {
    const auto result = load_heartbeat_md("/nonexistent/path/HEARTBEAT.md");
    CHECK_FALSE(result.has_value());
};

TEST_CASE("empty_path_returns_nullopt") {
    const auto result = load_heartbeat_md("");
    CHECK_FALSE(result.has_value());
};

TEST_CASE("loads_markdown_file_contents") {
    const auto path = orangutan::testing::unique_test_path("heartbeat-md", "HEARTBEAT-load.md");
    INFO("expected heartbeat fixture file to be written");
    REQUIRE(write_test_file(path, "# heartbeat\nready\n"));

    const auto result = load_heartbeat_md(path);

    INFO("expected heartbeat markdown to load");
    REQUIRE(result.has_value());
    CHECK(*result == "# heartbeat\nready\n");
};

TEST_CASE("ignores_non_markdown_extension") {
    const auto path = orangutan::testing::unique_test_path("heartbeat-md", "HEARTBEAT.txt");
    INFO("expected non-markdown fixture file to be written");
    REQUIRE(write_test_file(path, "not markdown"));

    const auto result = load_heartbeat_md(path);

    CHECK_FALSE(result.has_value());
};

TEST_CASE("returns_nullopt_when_file_cannot_be_opened") {
    const auto path = orangutan::testing::unique_test_path("heartbeat-md", "missing/HEARTBEAT.md");
    std::filesystem::remove_all(path.parent_path());

    const auto result = load_heartbeat_md(path);

    CHECK_FALSE(result.has_value());
};

} // namespace
} // namespace orangutan
