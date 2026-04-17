#include "test-helpers.hpp"

#include "tools/internal.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRegistry make_write_registry(const std::filesystem::path &workspace) {
        ToolRegistry registry;
        register_write_tool(registry, workspace);
        return registry;
    }

    std::string read_text(const std::filesystem::path &path) {
        return fileio::read_file(path);
    }

} // namespace

TEST_CASE("write creates parent directories and writes content") {
    const auto workspace = testing::unique_test_root("write-single");
    auto registry = make_write_registry(workspace);

    const auto target = workspace / "nested/dir/hello.txt";
    const auto result = registry.execute(ToolUse("w", "write", {{"path", target.string()}, {"content", "hi"}}));

    REQUIRE_FALSE(result.is_error);
    REQUIRE(std::filesystem::exists(target));
    CHECK(read_text(target) == "hi");
    CHECK(result.content.contains("Wrote 2 bytes"));
}

TEST_CASE("write overwrites an existing file") {
    const auto workspace = testing::unique_test_root("write-overwrite");
    auto registry = make_write_registry(workspace);

    const auto target = workspace / "note.txt";
    fileio::write_file(target, "original");

    const auto result = registry.execute(ToolUse("w", "write", {{"path", target.string()}, {"content", "replaced"}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(read_text(target) == "replaced");
}

TEST_CASE("write accepts empty content") {
    const auto workspace = testing::unique_test_root("write-empty");
    auto registry = make_write_registry(workspace);

    const auto target = workspace / "empty.txt";
    const auto result = registry.execute(ToolUse("w", "write", {{"path", target.string()}, {"content", ""}}));
    REQUIRE_FALSE(result.is_error);
    REQUIRE(std::filesystem::exists(target));
    CHECK(std::filesystem::file_size(target) == 0);
}

TEST_CASE("write batch form writes every file and reports each one") {
    const auto workspace = testing::unique_test_root("write-batch");
    auto registry = make_write_registry(workspace);

    nlohmann::json files = nlohmann::json::array();
    for (int i = 0; i < 6; ++i) {
        files.push_back({{"path", (workspace / ("f-" + std::to_string(i) + ".txt")).string()},
                         {"content", std::string("body-") + std::to_string(i)}});
    }

    const auto result = registry.execute(ToolUse("w", "write", {{"files", files}}));
    REQUIRE_FALSE(result.is_error);

    for (int i = 0; i < 6; ++i) {
        const auto path = workspace / ("f-" + std::to_string(i) + ".txt");
        REQUIRE(std::filesystem::exists(path));
        CHECK(read_text(path) == std::string("body-") + std::to_string(i));
        CHECK(result.content.contains(path.string()));
    }
}

TEST_CASE("write rejects mixing single path with files array") {
    const auto workspace = testing::unique_test_root("write-mixed");
    auto registry = make_write_registry(workspace);

    nlohmann::json input = {{"path", (workspace / "a.txt").string()},
                            {"content", "x"},
                            {"files", nlohmann::json::array({{{"path", (workspace / "b.txt").string()}, {"content", "y"}}})}};
    const auto result = registry.execute(ToolUse("w", "write", std::move(input)));
    CHECK(result.is_error);
    CHECK(result.content.contains("either"));
}

TEST_CASE("write rejects batch entries missing path or content") {
    const auto workspace = testing::unique_test_root("write-bad-entry");
    auto registry = make_write_registry(workspace);

    nlohmann::json input = {{"files",
                             nlohmann::json::array({
                                 nlohmann::json{{"path", (workspace / "ok.txt").string()}, {"content", "good"}},
                                 nlohmann::json{{"path", (workspace / "no-content.txt").string()}},
                             })}};
    const auto result = registry.execute(ToolUse("w", "write", std::move(input)));
    CHECK(result.is_error);
    CHECK(result.content.contains("`path` and `content`"));
}

TEST_CASE("write denies paths that escape the workspace sandbox") {
    const auto workspace = testing::unique_test_root("write-escape");
    auto registry = make_write_registry(workspace);

    const auto result = registry.execute(ToolUse("w", "write", {{"path", "/etc/orangutan-test.txt"}, {"content", "nope"}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("escapes workspace sandbox"));
}

TEST_CASE("write check_permissions validates every entry in the files array") {
    const auto workspace = testing::unique_test_root("write-perm");
    auto registry = make_write_registry(workspace);

    const auto *tool = registry.find_tool("write");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->check_permissions);
    const ToolPermissionContext ctx;

    SECTION("non-object entry is rejected") {
        const ToolUse call("w", "write", {{"files", nlohmann::json::array({"bare-string"})}});
        const auto decision = tool->check_permissions(call, ctx);
        CHECK_FALSE(decision.is_passthrough);
        REQUIRE(decision.message.has_value());
        CHECK(decision.message->contains("must be an object"));
    }

    SECTION("out-of-workspace path in one entry denies the whole call") {
        const ToolUse call("w", "write",
                           {{"files",
                             nlohmann::json::array({{{"path", (workspace / "ok.txt").string()}, {"content", "a"}},
                                                   {{"path", "/etc/evil.txt"}, {"content", "b"}}})}});
        const auto decision = tool->check_permissions(call, ctx);
        CHECK_FALSE(decision.is_passthrough);
    }
}
