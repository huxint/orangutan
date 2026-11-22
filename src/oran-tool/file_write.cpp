#include <oran/tool/builtins.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/tool/registry.hpp>

#include "_impl/parse_input.hpp"
#include "version_token.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileWriteSchema =
    R"({"type":"object","properties":{)"
    R"("path":{"type":"string","minLength":1,"description":"Absolute or workspace-relative file path."},)"
    R"("content":{"type":"string","description":"Complete UTF-8 file contents for truncate or fail_if_exists; only the added text for append. Empty is allowed. No diff markers or read metadata."},)"
    R"("mode":{"type":"string","enum":["truncate","append","fail_if_exists"],"default":"truncate","description":"truncate replaces the whole file; append adds content; fail_if_exists creates only if absent."},)"
    R"("create_parents":{"type":"boolean","default":false,"description":"Create missing parent directories."},)"
    R"("max_bytes":{"type":"integer","minimum":1,"maximum":16777216,"default":16777216,"description":"UTF-8 byte cap for supplied content, not the final size of an appended file."},)"
    R"("expected_version":{"type":"string","description":"Opaque fingerprint copied from FileRead of this existing file. Rejects a changed or missing file; omit when creating a new file."}},)"
    R"("required":["path","content"],"additionalProperties":false})";

struct FileWriteRequest {
  std::string content;
  io::WriteTextOptions options;
  std::optional<std::string> expected_version;
};

[[nodiscard]] async::Awaitable<core::Result<Output>> file_write_handler(FileWriteRequest request,
                                                                       DispatchContext& ctx) {
  auto& [content, options, expected_version] = request;
  const auto byte_count = content.size();
  if (!ctx.resolved_path.has_value() || !ctx.resolved_path->authority.has_value()) {
    co_return std::unexpected(core::Error::internal("FileWrite requires a resolved workspace authority"));
  }
  const auto& path = ctx.resolved_path->absolute_path;
  auto mutation = ctx.resolved_path->authority->begin_file_mutation(ctx.resolved_path->authority_relative_path,
                                                                    options.create_parent_directories);
  if (!mutation) {
    co_return std::unexpected(std::move(mutation).error());
  }

  // A missing target also invalidates the caller's expected version.
  if (expected_version) {
    auto opened = mutation->open_existing();
    core::Result<io::FileFingerprint> pre;
    if (opened) {
      pre = io::compute_file_fingerprint(*opened);
    } else {
      pre = std::unexpected(std::move(opened).error());
    }
    if (!pre) {
      co_return std::unexpected(core::Error{core::ErrorKind::conflict, "FileWrite: expected_version cannot be verified"}
                                    .with("path", path)
                                    .with("reason", "stale_fingerprint")
                                    .with("detail", std::string{pre.error().message()}));
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*expected_version != current_token) {
      co_return std::unexpected(
          core::Error{core::ErrorKind::conflict, "FileWrite: file has changed since the expected version"}
              .with("path", path)
              .with("reason", "stale_fingerprint")
              .with("expected", *expected_version)
              .with("fingerprint", current_token));
    }
  }

  if (options.atomic && expected_version) {
    // Re-verify the token in the commit critical section so a file changed
    // between the pre-write check and the rename cannot be clobbered.
    options.verify_before_commit = detail::expected_version_verifier(path, *expected_version);
  }
  auto written =
      co_await io::write_text_file(ctx.executor, std::move(*mutation), std::move(content), std::move(options));
  if (!written) {
    co_return std::unexpected(std::move(written).error());
  }
  co_return Output{
      .text = std::format("wrote {} bytes to {}", byte_count, path),
      .usage =
          ToolUsage{
              .bytes_written = byte_count,
              .files_touched = 1,
          },
  };
}

[[nodiscard]] core::Result<PreparedCall> prepare_file_write(std::string_view input_json) {
  constexpr auto fields = std::to_array<std::string_view>({
      "path", "content", "mode", "create_parents", "max_bytes", "expected_version"});
  auto parsed = detail::parse_input_object(input_json, kFileWriteName, fields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_path_field(*parsed, kFileWriteName);
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }
  auto content_field = detail::require_string_field(*parsed, kFileWriteName, "content");
  if (!content_field) {
    return std::unexpected(std::move(content_field).error());
  }

  auto max_bytes = detail::parse_file_max_bytes(*parsed, kFileWriteName);
  if (!max_bytes) {
    return std::unexpected(std::move(max_bytes).error());
  }

  io::WriteTextOptions options{};
  if (parsed->contains("mode")) {
    if (!(*parsed)["mode"].is_string()) {
      return std::unexpected(core::Error::invalid_argument("FileWrite: `mode` must be a string"));
    }
    const auto mode_text = (*parsed)["mode"].get<std::string>();
    auto mode = core::parse_enum<io::WriteMode>(mode_text);
    if (!mode.has_value()) {
      return std::unexpected(
          core::Error::invalid_argument("FileWrite: `mode` must be one of truncate|append|fail_if_exists")
              .with("value", mode_text));
    }
    options.mode = *mode;
  }
  if (parsed->contains("create_parents")) {
    if (!(*parsed)["create_parents"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileWrite: `create_parents` must be a boolean"));
    }
    options.create_parent_directories = (*parsed)["create_parents"].get<bool>();
  }

  std::optional<std::string> expected_version;
  if (parsed->contains("expected_version")) {
    if (!(*parsed)["expected_version"].is_string()) {
      return std::unexpected(core::Error::invalid_argument("FileWrite: `expected_version` must be a string"));
    }
    expected_version = (*parsed)["expected_version"].get<std::string>();
  }

  auto path = *std::move(path_field);
  auto content = *std::move(content_field);
  const auto byte_count = content.size();
  if (static_cast<std::uintmax_t>(byte_count) > *max_bytes) {
    return std::unexpected(core::Error::invalid_argument("FileWrite: `content` exceeds max_bytes")
                                  .with("path", path)
                                  .with("content_bytes", std::to_string(byte_count))
                                  .with("max_bytes", std::to_string(*max_bytes)));
  }

  options.atomic = options.mode == io::WriteMode::truncate;
  return PreparedCall{
      .path = PathRequest{.path = std::move(path),
                          .intent = PathIntent::write,
                          .write_intent = WriteIntent{.create_parent_directories = options.create_parent_directories}},
      .execute = [request = FileWriteRequest{std::move(content), std::move(options), std::move(expected_version)}](
                     DispatchContext& ctx) mutable { return file_write_handler(std::move(request), ctx); },
  };
}

}  // namespace

core::Result<void> register_file_write(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileWriteName},
      .description = "Create a UTF-8 text file, append text, or replace its complete contents. Prefer FileEdit for "
                     "targeted changes. The default truncate mode overwrites existing content: read existing files "
                     "first and pass their fingerprint as expected_version. For new files, use fail_if_exists to "
                     "avoid overwriting an existing file. On a version conflict, read and reconcile current content "
                     "before retrying. Returns the written byte count; it does not validate the resulting program.",
      .input_schema_json = std::string{kFileWriteSchema},
      .required_capabilities = {core::Capability::write_file},
  };
  return registry.add_prepared(std::move(def), &prepare_file_write);
}

}  // namespace orangutan::tool
