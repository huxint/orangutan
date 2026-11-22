// src/oran-tool/schema_validation.cpp — JSON Schema sanity checks.

#include "_impl/schema_validation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>

namespace orangutan::tool::detail {

namespace {

using json = ::nlohmann::json;

constexpr auto kJsonSchemaTypes = std::array<std::string_view, 7>{
    "array",
    "boolean",
    "integer",
    "null",
    "number",
    "object",
    "string",
};

[[nodiscard]] core::Error schema_error(std::string message, std::string_view tool, std::string path) {
  return core::Error::invalid_argument(std::move(message))
      .with("tool", std::string{tool})
      .with("schema_path", std::move(path));
}

[[nodiscard]] std::string child_path(std::string_view base, std::string_view child) {
  if (base.empty() || base == "$") {
    return std::format("$.{}", child);
  }
  return std::format("{}.{}", base, child);
}

[[nodiscard]] std::string element_path(std::string_view base, std::size_t index) {
  return std::format("{}[{}]", base, index);
}

[[nodiscard]] bool is_known_schema_type(std::string_view value) {
  return std::ranges::contains(kJsonSchemaTypes, value);
}

[[nodiscard]] core::Result<void> validate_schema_node(const json& schema, std::string_view tool, std::string_view path);

[[nodiscard]] core::Result<void> validate_schema_type(const json& value, std::string_view tool, std::string_view path) {
  if (value.is_string()) {
    const auto type = value.get<std::string>();
    if (!is_known_schema_type(type)) {
      return std::unexpected(
          schema_error("tool input_schema_json has an unknown JSON Schema type", tool, std::string{path})
              .with("value", type));
    }
    return {};
  }

  if (!value.is_array() || value.empty()) {
    return std::unexpected(schema_error("tool input_schema_json `type` must be a string or a non-empty string array",
                                        tool,
                                        std::string{path}));
  }

  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto item_path = element_path(path, i);
    if (!value[i].is_string()) {
      return std::unexpected(
          schema_error("tool input_schema_json `type` array entries must be strings", tool, item_path));
    }
    const auto type = value[i].get<std::string>();
    if (!is_known_schema_type(type)) {
      return std::unexpected(
          schema_error("tool input_schema_json has an unknown JSON Schema type", tool, item_path).with("value", type));
    }
  }
  return {};
}

[[nodiscard]] core::Result<void>
validate_properties(const json& properties, std::string_view tool, std::string_view path) {
  if (!properties.is_object()) {
    return std::unexpected(
        schema_error("tool input_schema_json `properties` must be an object", tool, std::string{path}));
  }
  for (const auto& [name, property_schema] : properties.items()) {
    if (name.empty()) {
      return std::unexpected(
          schema_error("tool input_schema_json property names must be non-empty", tool, child_path(path, name)));
    }
    if (auto valid = validate_schema_node(property_schema, tool, child_path(path, name)); !valid) {
      return valid;
    }
  }
  return {};
}

[[nodiscard]] core::Result<void>
validate_required(const json& required, const json* properties, std::string_view tool, std::string_view path) {
  if (!required.is_array()) {
    return std::unexpected(schema_error("tool input_schema_json `required` must be an array", tool, std::string{path}));
  }
  for (std::size_t i = 0; i < required.size(); ++i) {
    const auto item_path = element_path(path, i);
    if (!required[i].is_string()) {
      return std::unexpected(
          schema_error("tool input_schema_json `required` entries must be strings", tool, item_path));
    }
    const auto name = required[i].get<std::string>();
    if (name.empty()) {
      return std::unexpected(
          schema_error("tool input_schema_json `required` entries must be non-empty", tool, item_path));
    }
    if (properties != nullptr && properties->is_object() && !properties->contains(name)) {
      return std::unexpected(
          schema_error("tool input_schema_json `required` entry is not declared in `properties`", tool, item_path)
              .with("property", name));
    }
  }
  return {};
}

[[nodiscard]] core::Result<void>
validate_schema_node(const json& schema, std::string_view tool, std::string_view path) {
  if (schema.is_boolean()) {
    return {};
  }
  if (!schema.is_object()) {
    return std::unexpected(
        schema_error("tool input_schema_json nodes must be JSON objects or booleans", tool, std::string{path}));
  }

  if (const auto it = schema.find("type"); it != schema.end()) {
    if (auto valid = validate_schema_type(*it, tool, child_path(path, "type")); !valid) {
      return valid;
    }
  }

  const json* properties = nullptr;
  if (const auto it = schema.find("properties"); it != schema.end()) {
    properties = &*it;
    if (auto valid = validate_properties(*it, tool, child_path(path, "properties")); !valid) {
      return valid;
    }
  }

  if (const auto it = schema.find("required"); it != schema.end()) {
    if (auto valid = validate_required(*it, properties, tool, child_path(path, "required")); !valid) {
      return valid;
    }
  }

  if (const auto it = schema.find("additionalProperties"); it != schema.end() && !it->is_boolean()) {
    if (auto valid = validate_schema_node(*it, tool, child_path(path, "additionalProperties")); !valid) {
      return valid;
    }
  }

  if (const auto it = schema.find("enum"); it != schema.end() && (!it->is_array() || it->empty())) {
    return std::unexpected(
        schema_error("tool input_schema_json `enum` must be a non-empty array", tool, child_path(path, "enum")));
  }

  for (const auto keyword : {"minimum", "maximum"}) {
    if (const auto it = schema.find(keyword); it != schema.end() && !it->is_number()) {
      return std::unexpected(
          schema_error("tool input_schema_json numeric bounds must be numbers", tool, child_path(path, keyword)));
    }
  }

  return {};
}

}  // namespace

core::Result<void> validate_input_schema(std::string_view tool, std::string_view schema_json) {
  if (schema_json.empty()) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json must not be empty")
                               .with("tool", std::string{tool})
                               .with("schema_path", "$"));
  }

  json parsed;
  try {
    parsed = json::parse(schema_json);
  } catch (const json::parse_error& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json is not valid JSON")
                               .with("tool", std::string{tool})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  } catch (const json::exception& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json is not valid JSON")
                               .with("tool", std::string{tool})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(core::Error::invalid_argument("tool input_schema_json validation failed")
                               .with("tool", std::string{tool})
                               .with("schema_path", "$")
                               .with("detail", e.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(schema_error("tool input_schema_json must be a JSON Schema object", tool, std::string{"$"}));
  }
  return validate_schema_node(parsed, tool, "$");
}

}  // namespace orangutan::tool::detail
