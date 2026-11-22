// src/oran-tool/input_redaction.cpp — hook-facing redacted input views.

#include "_impl/input_redaction.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/permission/approval.hpp>
#include <oran/permission/audit.hpp>
#include <oran/tool/builtins.hpp>

namespace orangutan::tool::detail {
namespace {

[[nodiscard]] std::string hash_input(std::string_view input_json) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(input_json));
}

[[nodiscard]] nlohmann::json base_redacted_input(std::string_view tool_name, std::string_view input_json) {
  auto out = nlohmann::json::object();
  out["kind"] = "redacted_tool_input";
  out["tool_name"] = std::string{tool_name};
  out["input_hash"] = hash_input(input_json);
  out["input_bytes"] = input_json.size();
  return out;
}

void add_string_byte_count(nlohmann::json& out,
                           const nlohmann::json& input,
                           std::string_view field_name,
                           std::string_view output_name) {
  const auto it = input.find(std::string{field_name});
  if (it != input.end() && it->is_string()) {
    out[std::string{output_name}] = it->get_ref<const std::string&>().size();
  }
}

void fill_file_write_counts(nlohmann::json& out, const nlohmann::json& input) {
  out["redacted_fields"] = nlohmann::json::array({"content"});
  add_string_byte_count(out, input, "content", "content_bytes");
}

void fill_file_edit_counts(nlohmann::json& out, const nlohmann::json& input) {
  out["redacted_fields"] = nlohmann::json::array({"old_string", "new_string"});
  add_string_byte_count(out, input, "old_string", "old_string_bytes");
  add_string_byte_count(out, input, "new_string", "new_string_bytes");
}

}  // namespace

std::optional<std::string> redacted_hook_input_json(std::string_view tool_name, std::string_view input_json) {
  if (tool_name != kFileWriteName && tool_name != kFileEditName && tool_name != kMemoryRememberName &&
      tool_name != AGENT_RUN_NAME && !is_team_tool(tool_name)) {
    return std::nullopt;
  }

  auto out = base_redacted_input(tool_name, input_json);
  if (tool_name == kMemoryRememberName || tool_name == AGENT_RUN_NAME || is_team_tool(tool_name)) {
    out["redaction_status"] = "ok";
    return out.dump();
  }

  try {
    const auto input = nlohmann::json::parse(input_json);
    if (!input.is_object()) {
      out["redaction_status"] = "input_json_not_object";
      return out.dump();
    }

    if (tool_name == kFileWriteName) {
      fill_file_write_counts(out, input);
    } else {
      fill_file_edit_counts(out, input);
    }
    out["redaction_status"] = "ok";
  } catch (const nlohmann::json::parse_error&) {
    out["redaction_status"] = "input_json_parse_failed";
  } catch (const std::exception&) {
    out["redaction_status"] = "input_json_unavailable";
  }
  return out.dump();
}

}  // namespace orangutan::tool::detail
