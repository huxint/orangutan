#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("test_tmp_root is scoped per test process") {
    const auto shared_root = std::filesystem::current_path() / "tmp" / "tests";
    const auto root = orangutan::testing::test_tmp_root();

    const auto shared_root_text = std::filesystem::weakly_canonical(shared_root).string();
    const auto root_text = std::filesystem::weakly_canonical(root).string();

    CHECK(root_text.starts_with(shared_root_text));
    CHECK(root != shared_root);
}
