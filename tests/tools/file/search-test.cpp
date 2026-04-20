#include "test-helpers.hpp"

#include "tools/file/search/common.hpp"
#include "tools/internal.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/escape.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRegistry make_search_registry(const std::filesystem::path &workspace) {
        ToolRegistry registry;
        register_fd_tool(registry, workspace);
        register_rg_tool(registry, workspace);
        return registry;
    }

} // namespace

TEST_CASE("search binary_available walks PATH via filesystem") {
    // /bin/sh is effectively universal on the test host, and `:/bogus-dir`
    // entries must not crash the walk.
    CHECK(file::search::binary_available("sh"));
    CHECK_FALSE(file::search::binary_available("definitely-not-a-binary-xyzzy"));
}

TEST_CASE("search shell_quote escapes single quotes safely") {
    using utils::shell_single_quote_escape;
    CHECK(shell_single_quote_escape("simple") == "'simple'");
    CHECK(shell_single_quote_escape("it's") == R"('it'\''s')");
    CHECK(shell_single_quote_escape("") == "''");
}

TEST_CASE("search missing_binary_error surfaces a hint") {
    const auto msg = file::search::missing_binary_error("widget", "Install via `apt install widget`.");
    CHECK(msg.contains("`widget`"));
    CHECK(msg.contains("PATH"));
    CHECK(msg.contains("Install"));
}

TEST_CASE("fd tool reports a missing binary without throwing") {
    if (file::search::binary_available("fd")) {
        SUCCEED("fd is installed — missing-binary branch not exercised here");
        return;
    }

    const auto workspace = testing::unique_test_root("fd-missing");
    auto registry = make_search_registry(workspace);

    const auto result = registry.execute(ToolUse("fd", "fd", {{"pattern", ".*"}}));
    REQUIRE_FALSE(result.is_error); // wrapped as normal output, not an exception
    CHECK(result.content.contains("`fd` binary not found"));
}

TEST_CASE("fd finds files by regex when the binary is available") {
    if (!file::search::binary_available("fd")) {
        SUCCEED("fd not installed — skipping happy-path test");
        return;
    }

    const auto workspace = testing::unique_test_root("fd-ok");
    fileio::write_file(workspace / "alpha.txt", "hi");
    fileio::write_file(workspace / "beta.log", "hi");

    auto registry = make_search_registry(workspace);
    const auto result = registry.execute(ToolUse("fd", "fd", {{"pattern", "alpha"}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("alpha.txt"));
    CHECK_FALSE(result.content.contains("beta.log"));
}

TEST_CASE("rg tool reports a missing binary without throwing") {
    if (file::search::binary_available("rg")) {
        SUCCEED("rg is installed — missing-binary branch not exercised here");
        return;
    }

    const auto workspace = testing::unique_test_root("rg-missing");
    auto registry = make_search_registry(workspace);

    const auto result = registry.execute(ToolUse("rg", "rg", {{"pattern", "anything"}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("`rg` binary not found"));
}

TEST_CASE("rg returns matches and distinguishes empty results") {
    if (!file::search::binary_available("rg")) {
        SUCCEED("rg not installed — skipping happy-path test");
        return;
    }

    const auto workspace = testing::unique_test_root("rg-ok");
    fileio::write_file(workspace / "note.txt", "keep this line\nskip that line\n");
    auto registry = make_search_registry(workspace);

    SECTION("finds matching lines") {
        const auto result = registry.execute(ToolUse("rg", "rg", {{"pattern", "keep"}}));
        REQUIRE_FALSE(result.is_error);
        CHECK(result.content.contains("keep this line"));
    }

    SECTION("treats no-match (exit 1) as a clean empty result") {
        const auto result = registry.execute(ToolUse("rg", "rg", {{"pattern", "definitely-missing-token"}}));
        REQUIRE_FALSE(result.is_error);
        CHECK_FALSE(result.content.contains("exited with code"));
    }
}
