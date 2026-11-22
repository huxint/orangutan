#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/result.hpp>

namespace orangutan::tool::detail {

/// Parse an object, rejecting unknown fields when an allowlist is supplied.
[[nodiscard]] core::Result<nlohmann::json> parse_input_object(
    std::string_view input_json, std::string_view tool_name, std::span<const std::string_view> allowed_fields = {});

/// Read a required string field from a parsed input object.
///
/// Error: `invalid_argument` with message
/// `"<tool_name>: input must include a string `<field>` field"`
/// when the field is missing or not a JSON string.
[[nodiscard]] core::Result<std::string>
require_string_field(const nlohmann::json& input, std::string_view tool_name, std::string_view field);

[[nodiscard]] core::Result<std::string> require_path_field(const nlohmann::json& input, std::string_view tool_name);

[[nodiscard]] core::Result<std::uintmax_t>
parse_positive_unsigned(const nlohmann::json& raw, std::string_view tool_name, std::string_view field);

[[nodiscard]] core::Result<std::uintmax_t> parse_file_max_bytes(const nlohmann::json& input, std::string_view tool_name);

}  // namespace orangutan::tool::detail
