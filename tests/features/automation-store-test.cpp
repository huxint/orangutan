#include "features/automation/store.hpp"

#include <gtest/gtest.h>

TEST(AutomationStoreTest, ConstructsWithExplicitPath) {
    orangutan::automation::Store store("/tmp/orangutan-automation-store-test.db");
    static_cast<void>(store);
    SUCCEED();
}
