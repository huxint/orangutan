#include "features/automation/store.hpp"

#include <gtest/gtest.h>

TEST(AutomationStoreTest, ConstructsWithExplicitPath) {
    orangutan::automation::Store store("/tmp/orangutan-automation-store-test.db");
    (void)store;
    SUCCEED();
}
