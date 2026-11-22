#include <oran/memory/session.hpp>

#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/storage/session_repository.hpp>

namespace orangutan::memory::session {
namespace {

using json = nlohmann::ordered_json;

[[nodiscard]] core::Error invalid_field(std::string field) {
  return core::Error::invalid_argument("memory session field must not be empty").with("field", std::move(field));
}

[[nodiscard]] core::Result<void> validate_key(std::string_view session_id, std::string_view agent_key) {
  if (session_id.empty()) {
    return std::unexpected(invalid_field("session_id"));
  }
  if (agent_key.empty()) {
    return std::unexpected(invalid_field("agent_key"));
  }
  return {};
}

[[nodiscard]] core::Error message_parse_error(std::string reason) {
  return core::Error::parsing("memory session message JSON is malformed").with("reason", std::move(reason));
}

void put_if_present(json& object, std::string_view key, const std::optional<std::string>& value) {
  if (value.has_value()) {
    object[std::string{key}] = *value;
  }
}

[[nodiscard]] json content_to_json(const core::Content& content) {
  return std::visit(
      [](const auto& block) -> json {
        using T = std::decay_t<decltype(block)>;
        if constexpr (std::same_as<T, core::TextContent>) {
          return json{{"type", "text"}, {"text", block.text}};
        } else if constexpr (std::same_as<T, core::ImageContent>) {
          return json{{"type", "image"}, {"media_type", block.media_type}, {"data_base64", block.data_base64}};
        } else if constexpr (std::same_as<T, core::ThinkingContent>) {
          auto out = json{{"type", "thinking"}, {"thinking", block.thinking}};
          put_if_present(out, "signature", block.signature);
          return out;
        } else if constexpr (std::same_as<T, core::ToolUseContent>) {
          return json{{"type", "tool_use"}, {"id", block.id}, {"name", block.name}, {"input_json", block.input_json}};
        } else {
          auto out = json{{"type", "tool_result"},
                          {"tool_use_id", block.tool_use_id},
                          {"output", block.output},
                          {"is_error", block.is_error}};
          put_if_present(out, "data_json", block.data_json);
          return out;
        }
      },
      content);
}

[[nodiscard]] std::string message_to_json(const core::Message& message) {
  auto blocks = json::array();
  for (const auto& block : message.blocks) {
    blocks.push_back(content_to_json(block));
  }

  auto out = json{
      {"version", 1},
      {"blocks", std::move(blocks)},
  };
  if (message.created_at.has_value()) {
    out["created_at"] = core::time::format_iso8601_utc(*message.created_at);
  }
  return out.dump();
}

[[nodiscard]] core::Result<std::string>
required_string(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return std::unexpected(message_parse_error(std::format("{}: expected string {}", context, key)));
  }
  return it->get<std::string>();
}

[[nodiscard]] core::Result<std::optional<std::string>>
optional_string(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return std::optional<std::string>{};
  }
  if (!it->is_string()) {
    return std::unexpected(message_parse_error(std::format("{}: expected string {}", context, key)));
  }
  return std::optional<std::string>{it->get<std::string>()};
}

[[nodiscard]] core::Result<bool> required_bool(const json& object, std::string_view key, std::string_view context) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_boolean()) {
    return std::unexpected(message_parse_error(std::format("{}: expected boolean {}", context, key)));
  }
  return it->get<bool>();
}

[[nodiscard]] core::Result<core::Content> content_from_json(const json& value, std::size_t index) {
  if (!value.is_object()) {
    return std::unexpected(message_parse_error("content block is not an object").with("index", std::to_string(index)));
  }

  const auto context = std::format("content[{}]", index);
  auto type = required_string(value, "type", context);
  if (!type) {
    return std::unexpected(std::move(type).error());
  }

  if (*type == "text") {
    auto text = required_string(value, "text", context);
    if (!text) {
      return std::unexpected(std::move(text).error());
    }
    return core::Content{core::TextContent{.text = std::move(*text)}};
  }

  if (*type == "thinking") {
    auto thinking = required_string(value, "thinking", context);
    if (!thinking) {
      return std::unexpected(std::move(thinking).error());
    }
    auto signature = optional_string(value, "signature", context);
    if (!signature) {
      return std::unexpected(std::move(signature).error());
    }
    return core::Content{core::ThinkingContent{.thinking = std::move(*thinking), .signature = std::move(*signature)}};
  }

  if (*type == "image") {
    auto media = required_string(value, "media_type", context);
    if (!media)
      return std::unexpected(media.error());
    auto data = required_string(value, "data_base64", context);
    if (!data)
      return std::unexpected(data.error());
    return core::Content{core::ImageContent{std::move(*media), std::move(*data)}};
  }

  if (*type == "tool_use") {
    auto id = required_string(value, "id", context);
    if (!id) {
      return std::unexpected(std::move(id).error());
    }
    auto name = required_string(value, "name", context);
    if (!name) {
      return std::unexpected(std::move(name).error());
    }
    auto input_json = required_string(value, "input_json", context);
    if (!input_json) {
      return std::unexpected(std::move(input_json).error());
    }
    return core::Content{
        core::ToolUseContent{.id = std::move(*id), .name = std::move(*name), .input_json = std::move(*input_json)}};
  }

  if (*type == "tool_result") {
    auto tool_use_id = required_string(value, "tool_use_id", context);
    if (!tool_use_id) {
      return std::unexpected(std::move(tool_use_id).error());
    }
    auto output = required_string(value, "output", context);
    if (!output) {
      return std::unexpected(std::move(output).error());
    }
    auto data_json = optional_string(value, "data_json", context);
    if (!data_json) {
      return std::unexpected(std::move(data_json).error());
    }
    auto is_error = required_bool(value, "is_error", context);
    if (!is_error) {
      return std::unexpected(std::move(is_error).error());
    }
    return core::Content{core::ToolResultContent{
        .tool_use_id = std::move(*tool_use_id),
        .output = std::move(*output),
        .data_json = std::move(*data_json),
        .is_error = *is_error,
    }};
  }

  return std::unexpected(message_parse_error("unknown content block type").with("type", std::move(*type)));
}

[[nodiscard]] core::Result<core::Message> message_from_json(const storage::SessionMessageRecord& record) {
  json parsed;
  try {
    parsed = json::parse(record.content_json);
  } catch (const json::parse_error& error) {
    return std::unexpected(message_parse_error("invalid JSON").with("detail", error.what()));
  }

  if (!parsed.is_object()) {
    return std::unexpected(
        message_parse_error("root is not an object").with("sequence", std::to_string(record.sequence)));
  }
  const auto version_it = parsed.find("version");
  if (version_it == parsed.end() || !version_it->is_number_integer() || version_it->get<int>() != 1) {
    return std::unexpected(
        message_parse_error("unsupported message JSON version").with("sequence", std::to_string(record.sequence)));
  }
  const auto blocks_it = parsed.find("blocks");
  if (blocks_it == parsed.end() || !blocks_it->is_array()) {
    return std::unexpected(
        message_parse_error("blocks is not an array").with("sequence", std::to_string(record.sequence)));
  }

  auto blocks = std::vector<core::Content>{};
  blocks.reserve(blocks_it->size());
  for (std::size_t i = 0; i < blocks_it->size(); ++i) {
    auto block = content_from_json((*blocks_it)[i], i);
    if (!block) {
      return std::unexpected(std::move(block).error().with("sequence", std::to_string(record.sequence)));
    }
    blocks.push_back(std::move(*block));
  }

  auto message = core::Message{
      .role = record.role,
      .blocks = std::move(blocks),
      .created_at = std::nullopt,
  };

  if (!record.created_at.empty()) {
    auto created = core::time::parse_iso8601_utc(record.created_at);
    if (!created) {
      return std::unexpected(std::move(created).error().with("sequence", std::to_string(record.sequence)));
    }
    message.created_at = *created;
  }

  return message;
}

[[nodiscard]] storage::SessionKey key_from(SessionId session_id, AgentKey agent_key) {
  return storage::SessionKey{.session_id = std::move(session_id.value), .agent_key = std::move(agent_key.value)};
}

}  // namespace

Store::Store(storage::SessionRepository& repository) noexcept : repository_{&repository} {}

async::Awaitable<core::Result<void>> Store::append(SessionId session_id, AgentKey agent_key, core::Message message) {
  co_return co_await append_all(std::move(session_id), std::move(agent_key), std::span{&message, 1});
}

async::Awaitable<core::Result<void>> Store::append_all(SessionId session_id,
                                                       AgentKey agent_key,
                                                       std::span<const core::Message> messages,
                                                       std::optional<ContextSnapshot> expected,
                                                       core::WorkingContext checkpoint) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto encoded = std::vector<storage::SessionMessageInput>{};
  encoded.reserve(messages.size());
  try {
    for (const auto& message : messages) {
      encoded.push_back(storage::SessionMessageInput{.role = message.role, .content_json = message_to_json(message)});
    }
  } catch (const json::exception&) {
    co_return std::unexpected(core::Error::parsing("memory session message cannot be serialized")
                                  .with("index", std::to_string(encoded.size())));
  }

  std::optional<storage::SessionCommit> commit;
  if (expected) {
    try {
      commit = storage::SessionCommit{.expected_messages = expected->message_count,
                                      .expected_revision = expected->checkpoint.revision,
                                      .checkpoint_json = json{{"covered_sequence", checkpoint.covered_sequence},
                                                              {"revision", checkpoint.revision},
                                                              {"summary", checkpoint.summary}}
                                                             .dump()};
    } catch (const json::exception&) {
      co_return std::unexpected(core::Error::parsing("checkpoint cannot be serialized"));
    }
  }
  auto appended = co_await repository_->append_messages(key_from(std::move(session_id), std::move(agent_key)),
                                                        std::move(encoded),
                                                        std::move(commit));
  if (!appended) {
    co_return std::unexpected(std::move(appended).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<ContextSnapshot>> Store::load_context(SessionId session_id, AgentKey agent_key) {
  auto record = co_await repository_->get_session(key_from(std::move(session_id), std::move(agent_key)));
  if (!record)
    co_return std::unexpected(record.error());
  if (!*record)
    co_return ContextSnapshot{};
  ContextSnapshot result{.checkpoint = {}, .message_count = (**record).message_count};
  try {
    auto metadata = json::parse((**record).metadata_json);
    if (!metadata.is_object())
      co_return std::unexpected(core::Error::storage("session metadata must be an object"));
    if (metadata.contains("oran_working_context")) {
      const auto& context = metadata.at("oran_working_context");
      if (!context.at("covered_sequence").is_number_integer() || !context.at("revision").is_number_integer())
        co_return std::unexpected(core::Error::storage("invalid checkpoint sequence or revision"));
      result.checkpoint = {.covered_sequence = context.at("covered_sequence").get<std::int64_t>(),
                           .revision = context.at("revision").get<std::int64_t>(),
                           .summary = context.at("summary").get<std::string>()};
      if (result.checkpoint.covered_sequence < 0 || result.checkpoint.covered_sequence > result.message_count ||
          result.checkpoint.revision < 0 || result.checkpoint.summary.size() > 65536 ||
          (result.checkpoint.covered_sequence > 0 && result.checkpoint.summary.empty()))
        co_return std::unexpected(core::Error::storage("invalid session checkpoint"));
    }
  } catch (const json::exception&) {
    co_return std::unexpected(core::Error::storage("invalid session checkpoint metadata"));
  }
  co_return result;
}

async::Awaitable<core::Result<std::vector<core::Message>>> Store::load_after(SessionId session_id,
                                                                             AgentKey agent_key,
                                                                             std::int64_t after,
                                                                             std::int64_t through,
                                                                             std::size_t max_bytes) {
  auto rows = co_await repository_->load_after(key_from(std::move(session_id), std::move(agent_key)),
                                               after,
                                               through,
                                               64,
                                               max_bytes);
  if (!rows)
    co_return std::unexpected(rows.error());
  std::vector<core::Message> messages;
  for (const auto& row : *rows) {
    auto message = message_from_json(row);
    if (!message)
      co_return std::unexpected(message.error());
    messages.push_back(std::move(*message));
  }
  co_return messages;
}

async::Awaitable<core::Result<std::vector<core::Message>>> Store::load(SessionId session_id, AgentKey agent_key) {
  if (auto valid = validate_key(session_id.value, agent_key.value); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto rows = co_await repository_->load_messages(key_from(std::move(session_id), std::move(agent_key)));
  if (!rows) {
    co_return std::unexpected(std::move(rows).error());
  }

  auto out = std::vector<core::Message>{};
  out.reserve(rows->size());
  for (const auto& row : *rows) {
    auto message = message_from_json(row);
    if (!message) {
      co_return std::unexpected(std::move(message).error());
    }
    out.push_back(std::move(*message));
  }
  co_return out;
}

async::Awaitable<core::Result<std::vector<core::Message>>>
Store::load_tail(SessionId session_id, AgentKey agent_key, std::size_t max_messages, std::size_t max_bytes) {
  auto rows =
      co_await repository_->load_tail(key_from(std::move(session_id), std::move(agent_key)), max_messages, max_bytes);
  if (!rows)
    co_return std::unexpected(std::move(rows).error());
  std::vector<core::Message> messages;
  messages.reserve(rows->size());
  for (const auto& row : *rows) {
    auto message = message_from_json(row);
    if (!message)
      co_return std::unexpected(std::move(message).error());
    messages.push_back(std::move(*message));
  }
  co_return messages;
}

}  // namespace orangutan::memory::session
