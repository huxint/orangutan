#include "infra/utf8.hpp"

#include <simdutf.h>
#include <uni_algo/ranges_conv.h>

namespace orangutan::utf8 {
    namespace {

        size_t next_code_point_boundary(std::string_view input, size_t raw_start) {
            auto view = una::ranges::utf8_view{input};
            for (auto it = view.begin(); it != view.end(); ++it) {
                const auto begin = static_cast<size_t>(it.begin() - input.begin());
                if (begin >= raw_start) {
                    return begin;
                }

                const auto end = static_cast<size_t>(it.end() - input.begin());
                if (end > raw_start) {
                    return end;
                }
            }

            return input.size();
        }

    } // namespace

    std::string sanitize(std::string_view input) {
        if (simdutf::validate_utf8(input.data(), input.size())) {
            return std::string(input);
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

    std::string sanitize_and_truncate_valid_prefix(std::string_view input, size_t max_bytes, bool append_ellipsis) {
        return truncate_valid_prefix(sanitize(input), max_bytes, append_ellipsis);
    }

    std::string truncate_valid_prefix(std::string_view input, size_t max_bytes, bool append_ellipsis) {
        if (input.size() <= max_bytes) {
            return std::string(input);
        }
        if (max_bytes == 0) {
            return {};
        }

        const std::string_view ellipsis = append_ellipsis && max_bytes > 3 ? std::string_view{"..."} : std::string_view{};
        const size_t limit = max_bytes - ellipsis.size();
        const auto candidate = input.substr(0, limit);
        const size_t valid_bytes = simdutf::trim_partial_utf8(candidate.data(), candidate.size());

        std::string result(input.substr(0, valid_bytes));
        result.append(ellipsis);
        return result;
    }

    std::string truncate_valid_suffix(std::string_view input, size_t max_bytes) {
        if (input.size() <= max_bytes) {
            return std::string(input);
        }
        if (max_bytes == 0) {
            return {};
        }

        const size_t start = next_code_point_boundary(input, input.size() - max_bytes);
        return std::string(input.substr(start));
    }

} // namespace orangutan::utf8
