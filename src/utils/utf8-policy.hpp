#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "types/base.hpp"

namespace orangutan::utf8_policy {

    enum class invalid_utf8_mode : base::u8 {
        reject,
        drop_invalid,
        replace_invalid,
    };

    enum class normalization_mode : base::u8 {
        none,
        nfc,
        nfkc,
        nfkc_casefold,
    };

    enum class control_character_mode : base::u8 {
        allow,
        strip,
        reject,
    };

    enum class bound_mode : base::u8 {
        none,
        prefix,
        suffix,
    };

    enum class canonicalize_error_code : base::u8 {
        invalid_utf8,
        disallowed_code_point,
    };

    struct CanonicalizeError {
        canonicalize_error_code code = canonicalize_error_code::invalid_utf8;
        std::size_t byte_offset = 0;
    };

    struct CanonicalizePolicy {
        invalid_utf8_mode invalid_utf8 = invalid_utf8_mode::drop_invalid;
        normalization_mode normalization = normalization_mode::none;
        control_character_mode control_characters = control_character_mode::allow;
        bound_mode boundary = bound_mode::none;
        std::size_t max_bytes = 0;
        bool append_ellipsis = false;
    };

    struct CanonicalizeResult {
        std::string value;
        bool changed = false;
        bool truncated = false;
    };

    /// Canonicalize UTF-8 according to the selected policy.
    [[nodiscard]]
    std::expected<CanonicalizeResult, CanonicalizeError> canonicalize(std::string_view input, const CanonicalizePolicy &policy);

    /// Policy preset for user-visible text.
    [[nodiscard]]
    CanonicalizePolicy display_policy(std::size_t max_bytes = 0, bool append_ellipsis = false);

    /// Policy preset for normalized search keys.
    [[nodiscard]]
    CanonicalizePolicy search_key_policy();

    /// Policy preset for strict identifier-like values.
    [[nodiscard]]
    CanonicalizePolicy identifier_policy(std::size_t max_bytes = 0);

} // namespace orangutan::utf8_policy
