#include <boost/ut.hpp>

int main(int argc, const char **argv) {
    return static_cast<int>(boost::ut::cfg<>.run({.argc = argc, .argv = argv}));
}
