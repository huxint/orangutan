#include "test-helpers.hpp"

#include "tools/internal.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRegistry make_file_registry(const std::filesystem::path &workspace) {
        ToolRegistry registry;
        register_read_tool(registry, workspace);
        register_write_tool(registry, workspace);
        register_edit_tool(registry, workspace);
        return registry;
    }

    std::string read_text(const std::filesystem::path &path) {
        std::ifstream stream(path);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

} // namespace

TEST_CASE("write tool writes a single file and creates parent directories") {
    const auto workspace = testing::unique_test_root("write-single");
    auto registry = make_file_registry(workspace);

    const auto target = workspace / "nested/dir/hello.txt";
    const auto result = registry.execute(ToolUse("w1", "write", {{"path", target.string()}, {"content", "hi"}}));

    REQUIRE_FALSE(result.is_error);
    REQUIRE(std::filesystem::exists(target));
    CHECK(read_text(target) == "hi");
}

TEST_CASE("write tool handles batch form concurrently") {
    const auto workspace = testing::unique_test_root("write-batch");
    auto registry = make_file_registry(workspace);

    nlohmann::json files = nlohmann::json::array();
    for (int i = 0; i < 8; ++i) {
        const auto path = workspace / ("batch-" + std::to_string(i) + ".txt");
        files.push_back({{"path", path.string()}, {"content", std::string("body-") + std::to_string(i)}});
    }

    const auto result = registry.execute(ToolUse("w2", "write", {{"files", files}}));
    REQUIRE_FALSE(result.is_error);

    for (int i = 0; i < 8; ++i) {
        const auto path = workspace / ("batch-" + std::to_string(i) + ".txt");
        REQUIRE(std::filesystem::exists(path));
        CHECK(read_text(path) == std::string("body-") + std::to_string(i));
    }
}

TEST_CASE("write tool rejects mixing single and batch forms") {
    const auto workspace = testing::unique_test_root("write-bad");
    auto registry = make_file_registry(workspace);

    nlohmann::json input = {{"path", (workspace / "a.txt").string()}, {"content", "x"},
                            {"files", nlohmann::json::array({{{"path", (workspace / "b.txt").string()}, {"content", "y"}}})}};
    const auto result = registry.execute(ToolUse("w3", "write", std::move(input)));
    CHECK(result.is_error);
}

TEST_CASE("read tool returns contents for a single file") {
    const auto workspace = testing::unique_test_root("read-single");
    auto registry = make_file_registry(workspace);

    const auto target = workspace / "note.txt";
    fileio::write_file(target, "first\nsecond\n");

    const auto result = registry.execute(ToolUse("r1", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("first"));
    CHECK(result.content.contains("second"));
}

TEST_CASE("read tool reads multiple paths in parallel and preserves order") {
    const auto workspace = testing::unique_test_root("read-batch");
    auto registry = make_file_registry(workspace);

    nlohmann::json paths = nlohmann::json::array();
    for (int i = 0; i < 5; ++i) {
        const auto path = workspace / ("f-" + std::to_string(i) + ".txt");
        fileio::write_file(path, "content-" + std::to_string(i));
        paths.push_back(path.string());
    }

    const auto result = registry.execute(ToolUse("r2", "read", {{"paths", paths}}));
    REQUIRE_FALSE(result.is_error);

    std::size_t last = 0;
    for (int i = 0; i < 5; ++i) {
        const auto needle = "content-" + std::to_string(i);
        const auto pos = result.content.find(needle, last);
        INFO("looking for " << needle << " after offset " << last);
        REQUIRE(pos != std::string::npos);
        last = pos;
    }
}

TEST_CASE("read tool isolates per-file errors in batch form") {
    const auto workspace = testing::unique_test_root("read-mixed");
    auto registry = make_file_registry(workspace);

    const auto existing = workspace / "exists.txt";
    fileio::write_file(existing, "ok");
    const auto missing = workspace / "missing.txt";

    const auto result = registry.execute(
        ToolUse("r3", "read", {{"paths", nlohmann::json::array({existing.string(), missing.string()})}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("ok"));
    CHECK(result.content.contains("missing.txt"));
}
