#include "features/tools/core/hashline.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace orangutan {
namespace {

constexpr std::string_view HASH_ALPHABET = "ZPMQVRWSNKTXJBYH";

struct HashDict {
    std::array<std::array<char, 2>, 256> entries;

    consteval HashDict() : entries{} {
        for (size_t i = 0; i < 256; ++i) {
            entries[i][0] = HASH_ALPHABET[i >> 4];
            entries[i][1] = HASH_ALPHABET[i & 0xF];
        }
    }
};

constexpr HashDict HASH_DICT{};

bool is_symbol_only(std::string_view line) {
    return std::ranges::none_of(line, [](unsigned char ch) {
        return std::isalnum(ch);
    });
}

std::string_view preprocess_line(std::string_view line) {
    // Strip \r
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    // Trim trailing whitespace
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace

std::string compute_line_hash(std::string_view line, size_t line_number) {
    const auto processed = preprocess_line(line);
    const auto seed = static_cast<XXH32_hash_t>(is_symbol_only(processed) ? line_number : 0);
    const auto hash = XXH32(processed.data(), processed.size(), seed) & 0xFF;
    const auto &entry = HASH_DICT.entries[hash];
    return {entry[0], entry[1]};
}

std::string format_hashline(std::string_view line, size_t line_number) {
    auto hash = compute_line_hash(line, line_number);
    return std::to_string(line_number) + "#" + hash + ":" + std::string(line);
}

} // namespace orangutan
