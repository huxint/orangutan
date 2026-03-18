#include "features/heartbeat/protocol/heartbeat-md.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string_view>

using namespace orangutan;

namespace {

std::filesystem::path make_test_path(std::string_view filename) {
    return std::filesystem::temp_directory_path() / filename;
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

} // namespace

TEST(HeartbeatMdTest, MissingFileReturnsNullopt) {
    auto result = load_heartbeat_md("/nonexistent/path/HEARTBEAT.md");
    EXPECT_FALSE(result.has_value());
}

TEST(HeartbeatMdTest, EmptyPathReturnsNullopt) {
    auto result = load_heartbeat_md("");
    EXPECT_FALSE(result.has_value());
}

TEST(HeartbeatMdTest, FileWithContentReturnsContent) {
    const auto path = make_test_path("orangutan-test-heartbeat.md");
    ASSERT_TRUE(write_test_file(path, "- [ ] Check server status\n- [ ] Review pending PRs\n"));

    auto result = load_heartbeat_md(path.string());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
    EXPECT_NE(result->find("Check server status"), std::string::npos);

    std::filesystem::remove(path);
}

TEST(HeartbeatMdTest, EmptyFileReturnsEmptyString) {
    const auto path = make_test_path("orangutan-test-heartbeat-empty.md");
    ASSERT_TRUE(write_test_file(path, ""));

    auto result = load_heartbeat_md(path.string());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());

    std::filesystem::remove(path);
}

TEST(HeartbeatMdTest, WhitespaceOnlyFileReturnsEmptyString) {
    const auto path = make_test_path("orangutan-test-heartbeat-ws.md");
    ASSERT_TRUE(write_test_file(path, "   \n\n  \t  \n"));

    auto result = load_heartbeat_md(path.string());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());

    std::filesystem::remove(path);
}

TEST(HeartbeatMdTest, HeadersOnlyFileReturnsEmptyString) {
    const auto path = make_test_path("orangutan-test-heartbeat-headers.md");
    ASSERT_TRUE(write_test_file(path, "#\n##\n###   \n"));

    auto result = load_heartbeat_md(path.string());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());

    std::filesystem::remove(path);
}
