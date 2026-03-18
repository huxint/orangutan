#include "features/tools/core/hashline.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <stdexcept>

namespace orangutan {

constexpr std::string_view HASH_ALPHABET = "ZPMQVRWSNKTXJBYH";

namespace {

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

Anchor parse_anchor(std::string_view anchor_str) {
    if (anchor_str.empty()) {
        throw std::runtime_error("anchor string is empty");
    }

    const auto sep = anchor_str.find('#');
    if (sep == std::string_view::npos) {
        throw std::runtime_error("anchor missing '#' separator");
    }

    const auto line_part = anchor_str.substr(0, sep);
    const auto hash_part = anchor_str.substr(sep + 1);

    // Parse line number
    size_t line_number = 0;
    auto [ptr, ec] = std::from_chars(line_part.data(), line_part.data() + line_part.size(), line_number);
    if (ec != std::errc{} || ptr != line_part.data() + line_part.size()) {
        throw std::runtime_error("anchor line number is not a valid integer");
    }
    if (line_number == 0) {
        throw std::runtime_error("anchor line number must be >= 1");
    }

    // Validate hash: exactly 2 chars from HASH_ALPHABET
    if (hash_part.size() != 2) {
        throw std::runtime_error("anchor hash must be exactly 2 characters");
    }
    for (char ch : hash_part) {
        if (HASH_ALPHABET.find(ch) == std::string_view::npos) {
            throw std::runtime_error("anchor hash contains invalid character");
        }
    }

    return Anchor{.line = line_number, .hash = std::string(hash_part)};
}

std::optional<HashMismatch> validate_anchor(const Anchor &anchor,
                                             const std::vector<std::string> &lines) {
    if (anchor.line < 1 || anchor.line > lines.size()) {
        return HashMismatch{.line = anchor.line, .expected = anchor.hash, .actual = ""};
    }

    const auto &content = lines[anchor.line - 1];
    auto actual_hash = compute_line_hash(content, anchor.line);

    if (actual_hash != anchor.hash) {
        return HashMismatch{.line = anchor.line, .expected = anchor.hash, .actual = actual_hash};
    }

    return std::nullopt;
}

} // namespace orangutan
