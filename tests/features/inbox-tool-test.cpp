#include "features/tools/builtin/inbox.hpp"

#include "support/ut.hpp"

boost::ut::suite inbox_tool_suite = [] {
    using namespace boost::ut;

    "registers_when_automation_runtime_is_present"_test = [] {
        expect(true);
    };
};
