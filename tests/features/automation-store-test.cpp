#include "features/automation/store.hpp"
#include "test-helpers.hpp"
#include "support/ut.hpp"

boost::ut::suite automation_store_suite = [] {
    using namespace boost::ut;

    "constructs_with_explicit_path"_test = [] {
        orangutan::automation::Store store(orangutan::testing::unique_test_db_path("automation-store", "store.db").string());
        static_cast<void>(store);
        expect(true);
    };
};
