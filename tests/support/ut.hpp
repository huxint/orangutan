#pragma once

#include <boost/ut.hpp>

// Boost.UT's `>> fatal` adapter works on UT expression types, not raw bools.
// Many tests use plain boolean preconditions (has_value, pointer checks, sqlite status, etc.).
// Bridge `bool >> fatal` to a native UT matcher expression so those checks stay fatal
// without reintroducing the old gtest-style compatibility layer.

inline auto operator>>(bool value, const boost::ut::detail::value_location<boost::ut::detail::fatal> &fatal_tag)
    -> decltype(boost::ut::operator>>((boost::ut::that % true == value), fatal_tag)) {
    return boost::ut::operator>>((boost::ut::that % true == value), fatal_tag);
}