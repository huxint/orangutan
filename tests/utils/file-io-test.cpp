#include "test-helpers.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace orangutan;

TEST_CASE("read_file reads complete regular file content") {
    const auto root = testing::unique_test_root("utils-file-io-grow");
    const auto path = root / "content.txt";

    std::string expected;
    expected.reserve(2048);
    for (int i = 0; i < 256; ++i) {
        expected += "line-" + std::to_string(i) + "\n";
    }
    fileio::write_file(path, expected);

    CHECK(fileio::read_file(path) == expected);

    std::filesystem::remove_all(root);
}

TEST_CASE("read_file supports procfs files with dynamic reported sizes") {
    const auto proc_path = std::filesystem::path{"/proc/self/cmdline"};
    if (!std::filesystem::exists(proc_path)) {
        SUCCEED("procfs not available on this platform");
        return;
    }

    CHECK_FALSE(fileio::read_file(proc_path).empty());
}
