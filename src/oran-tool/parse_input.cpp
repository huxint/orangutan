// src/oran-tool/parse_input.cpp — shared JSON input parsing for built-ins.

#include "_impl/parse_input.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>

namespace orangutan::tool::detail {

core::Result<nlohmann::json> parse_input_object(std::string_view input_json,
                                              std::string_view tool_name,
                                              std::span<const std::string_view> allowed_fields) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(input_json);
  } catch (const nlohmann::json::parse_error& e) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input is not valid JSON", tool_name)).with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input is not valid JSON", tool_name)).with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(core::Error::invalid_argument(std::format("{}: input must be a JSON object", tool_name)));
  }
  if (!allowed_fields.empty()) {
    for (const auto& [key, value] : parsed.items()) {
      if (!std::ranges::contains(allowed_fields, key)) {
        return std::unexpected(core::Error::invalid_argument(std::format("{}: unknown input field", tool_name))
                                   .with("field", key));
      }
    }
  }
  return parsed;
}

core::Result<std::string>
require_string_field(const nlohmann::json& input, std::string_view tool_name, std::string_view field) {
  const auto it = input.find(field);
  if (it == input.end() || !it->is_string()) {
    return std::unexpected(
        core::Error::invalid_argument(std::format("{}: input must include a string `{}` field", tool_name, field)));
  }
  return it->get<std::string>();
}

core::Result<std::string> require_path_field(const nlohmann::json& input, std::string_view tool_name) {
  auto path = require_string_field(input, tool_name, "path");
  if (path && (path->empty() || path->contains('\0'))) {
    return std::unexpected(core::Error::invalid_argument(
        std::format("{}: `path` must be non-empty and contain no NUL bytes", tool_name)));
  }
  return path;
}

core::Result<std::uintmax_t>
parse_positive_unsigned(const nlohmann::json& raw, std::string_view tool_name, std::string_view field) {
  if (!raw.is_number_integer()) {
    return std::unexpected(core::Error::invalid_argument(
        std::format("{}: `{}` must be a positive integer", tool_name, field)));
  }
  if (!raw.is_number_unsigned() && raw.get<std::int64_t>() <= 0) {
    return std::unexpected(core::Error::invalid_argument(std::format("{}: `{}` must be positive", tool_name, field))
                               .with("value", std::to_string(raw.get<std::int64_t>())));
  }
  const auto value = raw.get<std::uint64_t>();
  if (value == 0) {
    return std::unexpected(core::Error::invalid_argument(std::format("{}: `{}` must be positive", tool_name, field)));
  }
  return static_cast<std::uintmax_t>(value);
}

core::Result<std::uintmax_t> parse_file_max_bytes(const nlohmann::json& input, std::string_view tool_name) {
  constexpr std::uintmax_t limit = 16U * 1024U * 1024U;
  const auto it = input.find("max_bytes");
  if (it == input.end()) {
    return limit;
  }
  auto value = parse_positive_unsigned(*it, tool_name, "max_bytes");
  if (value && *value > limit) {
    return std::unexpected(core::Error::invalid_argument(std::format("{}: `max_bytes` must be <= 16777216", tool_name))
                               .with("value", std::to_string(*value))
                               .with("max_bytes", std::to_string(limit)));
  }
  return value;
}

}  // namespace orangutan::tool::detail
