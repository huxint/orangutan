#include "utils/utf8-policy.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <simdutf.h>
#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/prop.h>
#include <uni_algo/ranges_conv.h>

namespace orangutan::utf8_policy {
    namespace {

        constexpr std::string_view REPLACEMENT_CHARACTER_UTF8 = "\xEF\xBF\xBD";

        std::size_t next_code_point_boundary(std::string_view input, std::size_t raw_start) {
            auto view = una::ranges::utf8_view{input};
            for (auto it = view.begin(); it != view.end(); ++it) {
                const auto begin = static_cast<std::size_t>(it.begin() - input.begin());
                if (begin >= raw_start) {
                    return begin;
                }

                const auto end = static_cast<std::size_t>(it.end() - input.begin());
                if (end > raw_start) {
                    return end;
                }
            }

            return input.size();
        }

        std::expected<std::string, CanonicalizeError> sanitize_invalid_utf8(std::string_view input, invalid_utf8_mode mode) {
            const auto first = simdutf::validate_utf8_with_errors(input.data(), input.size());
            if (first.error == simdutf::error_code::SUCCESS) {
                return std::string(input);
            }

            if (mode == invalid_utf8_mode::reject) {
                return std::unexpected(CanonicalizeError{.code = canonicalize_error_code::invalid_utf8, .byte_offset = first.count});
            }

            std::string result;
            result.reserve(input.size());

            auto remaining = input;
            while (!remaining.empty()) {
                const auto validation = simdutf::validate_utf8_with_errors(remaining.data(), remaining.size());
                if (validation.error == simdutf::error_code::SUCCESS) {
                    result.append(remaining);
                    break;
                }

                result.append(remaining.substr(0, validation.count));
                if (mode == invalid_utf8_mode::replace_invalid) {
                    result.append(REPLACEMENT_CHARACTER_UTF8);
                }

                if (validation.error == simdutf::error_code::TOO_SHORT) {
                    break;
                }
                if (validation.count >= remaining.size()) {
                    break;
                }

                remaining.remove_prefix(validation.count + 1);
            }

            return result;
        }

        std::string normalize_utf8(std::string_view input, normalization_mode mode) {
            switch (mode) {
                case normalization_mode::none:
                    return std::string(input);
                case normalization_mode::nfc:
                    return una::norm::to_nfc_utf8(input);
                case normalization_mode::nfkc:
                    return una::norm::to_nfkc_utf8(input);
                case normalization_mode::nfkc_casefold:
                    return una::cases::to_casefold_utf8(una::norm::to_nfkc_utf8(input));
            }

            std::unreachable();
        }

        std::expected<std::string, CanonicalizeError> handle_control_characters(std::string_view input, control_character_mode mode) {
            if (mode == control_character_mode::allow) {
                return std::string(input);
            }

            std::string result;
            result.reserve(input.size());

            auto view = una::ranges::utf8_view{input};
            for (auto it = view.begin(); it != view.end(); ++it) {
                const auto code_point = static_cast<char32_t>(*it);
                const auto begin = static_cast<std::size_t>(it.begin() - input.begin());
                const auto end = static_cast<std::size_t>(it.end() - input.begin());
                if (una::codepoint::is_control(code_point)) {
                    if (mode == control_character_mode::reject) {
                        return std::unexpected(CanonicalizeError{.code = canonicalize_error_code::disallowed_code_point, .byte_offset = begin});
                    }
                    continue;
                }

                result.append(input.substr(begin, end - begin));
            }

            return result;
        }

        CanonicalizeResult apply_bounds(std::string_view input, bound_mode mode, std::size_t max_bytes, bool append_ellipsis) {
            if (mode == bound_mode::none) {
                return {.value = std::string(input), .changed = false, .truncated = false};
            }

            if (input.size() <= max_bytes) {
                return {.value = std::string(input), .changed = false, .truncated = false};
            }

            if (max_bytes == 0) {
                return {.value = {}, .changed = !input.empty(), .truncated = !input.empty()};
            }

            if (mode == bound_mode::prefix) {
                const std::string_view ellipsis = append_ellipsis && max_bytes > 3 ? std::string_view{"..."} : std::string_view{};
                const std::size_t limit = max_bytes - ellipsis.size();
                const auto candidate = input.substr(0, limit);
                const std::size_t valid_bytes = simdutf::trim_partial_utf8(candidate.data(), candidate.size());

                std::string bounded(input.substr(0, valid_bytes));
                bounded.append(ellipsis);
                return {.value = std::move(bounded), .changed = true, .truncated = true};
            }

            const std::size_t start = next_code_point_boundary(input, input.size() - max_bytes);
            return {.value = std::string(input.substr(start)), .changed = true, .truncated = true};
        }

    } // namespace

    std::expected<CanonicalizeResult, CanonicalizeError> canonicalize(std::string_view input, const CanonicalizePolicy &policy) {
        auto utf8_clean = sanitize_invalid_utf8(input, policy.invalid_utf8);
        if (!utf8_clean.has_value()) {
            return std::unexpected(utf8_clean.error());
        }

        std::string normalized = normalize_utf8(*utf8_clean, policy.normalization);
        auto control_handled = handle_control_characters(normalized, policy.control_characters);
        if (!control_handled.has_value()) {
            return std::unexpected(control_handled.error());
        }

        auto bounded = apply_bounds(*control_handled, policy.boundary, policy.max_bytes, policy.append_ellipsis);
        bounded.changed = bounded.changed || bounded.value != input;
        return bounded;
    }

    CanonicalizePolicy display_policy(std::size_t max_bytes, bool append_ellipsis) {
        CanonicalizePolicy policy{};
        policy.invalid_utf8 = invalid_utf8_mode::drop_invalid;
        policy.normalization = normalization_mode::none;
        policy.control_characters = control_character_mode::allow;
        if (max_bytes > 0) {
            policy.boundary = bound_mode::prefix;
            policy.max_bytes = max_bytes;
            policy.append_ellipsis = append_ellipsis;
        }
        return policy;
    }

    CanonicalizePolicy search_key_policy() {
        CanonicalizePolicy policy{};
        policy.invalid_utf8 = invalid_utf8_mode::drop_invalid;
        policy.normalization = normalization_mode::nfkc_casefold;
        policy.control_characters = control_character_mode::strip;
        return policy;
    }

    CanonicalizePolicy identifier_policy(std::size_t max_bytes) {
        CanonicalizePolicy policy{};
        policy.invalid_utf8 = invalid_utf8_mode::reject;
        policy.normalization = normalization_mode::none;
        policy.control_characters = control_character_mode::reject;
        if (max_bytes > 0) {
            policy.boundary = bound_mode::prefix;
            policy.max_bytes = max_bytes;
        }
        return policy;
    }

} // namespace orangutan::utf8_policy
