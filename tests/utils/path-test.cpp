#include "test-helpers.hpp"
#include "utils/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace orangutan;

TEST_CASE("normalize_path canonicalizes existing paths") {
    const auto root = testing::unique_test_root("utils-path-normalize");
    const auto existing = root / "alpha" / "beta";
    std::filesystem::create_directories(existing);

    const auto path = root / "alpha" / ".." / "alpha" / "beta";
    CHECK(utils::normalize_path(path) == std::filesystem::weakly_canonical(path));

    std::filesystem::remove_all(root);
}

TEST_CASE("expand_home_path expands tilde-prefixed paths") {
    const auto home = testing::unique_test_root("utils-path-home");
    testing::ScopedEnvVar home_env("HOME", home.string());

    CHECK(utils::expand_home_path(std::filesystem::path{"~"}) == home);
    CHECK(utils::expand_home_path(std::filesystem::path{"~/notes/todo.txt"}) == home / "notes" / "todo.txt");
    CHECK(utils::expand_home_path(std::filesystem::path{"relative.txt"}) == std::filesystem::path{"relative.txt"});

    std::filesystem::remove_all(home);
}

TEST_CASE("path_has_prefix compares full path components") {
    const auto root = utils::normalize_path(testing::unique_test_root("utils-path-prefix"));
    const auto inside = utils::normalize_path(root / "nested" / ".." / "file.txt");
    const auto sibling = utils::normalize_path(root.parent_path() / (root.filename().string() + "-other") / "file.txt");

    CHECK(utils::path_has_prefix(inside, root));
    CHECK_FALSE(utils::path_has_prefix(sibling, root));

    std::filesystem::remove_all(root);
}

TEST_CASE("resolve_relative_to joins relative paths against root") {
    const auto root = testing::unique_test_root("utils-path-resolve");
    const auto absolute = root / "nested" / ".." / "file.txt";

    CHECK(utils::resolve_relative_to(std::filesystem::path{"nested/../file.txt"}, root) == utils::normalize_path(root / "file.txt"));
    CHECK(utils::resolve_relative_to(absolute, root) == utils::normalize_path(absolute));

    std::filesystem::remove_all(root);
}
