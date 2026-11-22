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
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>
#include <oran/io/range.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

#include "_impl/parse_input.hpp"
#include "version_token.hpp"

namespace orangutan::tool {

namespace {

constexpr std::string_view kFileReadSchema =
    R"({"type":"object","properties":{)"
    R"("path":{"type":"string","minLength":1,"description":"Absolute or workspace-relative file path."},)"
    R"("offset":{"type":"integer","minimum":1,"default":1,"description":"First line to read, 1-based. Use the returned continuation offset for the next window."},)"
    R"("limit":{"type":"integer","minimum":1,"maximum":2000,"default":2000,"description":"Maximum lines to read."},)"
    R"("max_bytes":{"type":"integer","minimum":1,"maximum":16777216,"default":16777216,"description":"Source UTF-8 byte cap for this window; separate tool-output limits may shorten the result."},)"
    R"("allow_outside_workspace":{"type":"boolean","default":false,"description":"Request an outside-root read through approval. This flag does not grant access."}},)"
    R"("required":["path"],"additionalProperties":false})";

struct FileReadRequest {
  std::string path;
  io::ReadTextOptions options;
};

[[nodiscard]] core::Result<io::ReadTextOptions> parse_options(const nlohmann::json& parsed) {
  auto max_bytes = detail::parse_file_max_bytes(parsed, kFileReadName);
  if (!max_bytes) {
    return std::unexpected(std::move(max_bytes).error());
  }
  io::FileRange::LineSpan span{.start_line = 1, .line_count = 2000};
  for (const auto field : {"offset", "limit"}) {
    const auto it = parsed.find(field);
    if (it == parsed.end()) {
      continue;
    }
    auto value = detail::parse_positive_unsigned(*it, kFileReadName, field);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    if (std::string_view{field} == "offset") {
      span.start_line = *value;
    } else {
      if (*value > 2000) {
        return std::unexpected(core::Error::invalid_argument("FileRead: `limit` must be <= 2000"));
      }
      span.line_count = *value;
    }
  }
  return io::ReadTextOptions{.max_bytes = *max_bytes, .range = io::FileRange{.lines = span}};
}

/// Header line embedded above the file body so the text-only `tool::Output`
/// can still surface the metadata callers need. Shape:
/// `<path>:<start_line>-<end_line> fingerprint=<token> bytes=<n>[ truncated]`.
[[nodiscard]] std::string
format_header(std::string_view path, const io::ReadTextResult& result, const std::string& token) {
  std::string header = std::format("{}:{}-{} fingerprint={} bytes={}",
                                   path,
                                   result.start_line,
                                   result.end_line,
                                   token,
                                   result.returned_bytes);
  if (result.truncated) {
    header.append(" truncated");
  }
  return header;
}

[[nodiscard]] std::string format_data_json(std::string_view path,
                                           std::string_view body,
                                           const io::ReadTextResult& result,
                                           const std::string& token) {
  return nlohmann::json{
      {"kind", "file_read"},
      {"path", std::string{path}},
      {"text", std::string{body}},
      {"fingerprint", token},
      {"start_line", result.start_line},
      {"end_line", result.end_line},
      {"returned_bytes", result.returned_bytes},
      {"truncated", result.truncated},
  }
      .dump();
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_read_handler(FileReadRequest request,
                                                                      DispatchContext& ctx) {
  auto& [path, options] = request;
  std::optional<io::ReadOnlyFile> authorized_file;
  if (ctx.resolved_path.has_value()) {
    if (!ctx.resolved_path->authority.has_value()) {
      co_return std::unexpected(core::Error::internal("FileRead: resolved workspace path is missing authority"));
    }
    path = ctx.resolved_path->absolute_path;
    auto opened = ctx.resolved_path->authority->open_file(io::AnchoredPath{
        .relative_path = ctx.resolved_path->authority_relative_path,
        .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
    });
    if (!opened) {
      co_return std::unexpected(std::move(opened).error());
    }
    authorized_file.emplace(std::move(*opened));
  } else if (ctx.workspace != nullptr) {
    co_return std::unexpected(
        core::Error::internal("FileRead: workspace dispatch did not provide a resolved authority"));
  }

  auto result = authorized_file.has_value()
                    ? co_await io::read_text_file_ranged(ctx.executor, std::move(*authorized_file), options)
                    : co_await io::read_text_file_ranged(ctx.executor, path, options);
  if (!result) {
    co_return std::unexpected(std::move(result).error());
  }

  const auto token = detail::version_token(path, result->fingerprint);
  auto header = format_header(path, *result, token);
  if (result->truncated) {
    header.append("; incomplete window: retry with a smaller limit or larger max_bytes before editing");
  } else if (result->end_line < result->start_line) {
    header.append("; end of file");
  } else if (result->end_line - result->start_line + 1 < options.range->lines->line_count) {
    header.append("; end of file");
  } else {
    header.append(std::format("; continue with offset={}", result->end_line + 1));
  }
  auto body = std::move(result->text);
  auto data_json = format_data_json(path, body, *result, token);
  std::string text = header;
  text.push_back('\n');
  text.append(body);
  co_return Output{
      .text = std::move(text),
      .data_json = std::move(data_json),
      .usage =
          ToolUsage{
              .bytes_read = result->returned_bytes,
              .files_touched = 1,
              .truncated = result->truncated,
          },
  };
}

[[nodiscard]] core::Result<PreparedCall> prepare_file_read(std::string_view input_json) {
  constexpr auto fields = std::to_array<std::string_view>({
      "path", "offset", "limit", "max_bytes",
      "allow_outside_workspace"});
  auto parsed = detail::parse_input_object(input_json, kFileReadName, fields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_path_field(*parsed, kFileReadName);
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }

  auto options = parse_options(*parsed);
  if (!options) {
    return std::unexpected(std::move(options).error());
  }

  if (parsed->contains("allow_outside_workspace") && !(*parsed)["allow_outside_workspace"].is_boolean()) {
    return std::unexpected(core::Error::invalid_argument("FileRead: `allow_outside_workspace` must be a boolean"));
  }

  return PreparedCall{
      .path = PathRequest{.path = *path_field,
                          .intent = PathIntent::read,
                          .allow_outside_workspace = parsed->value("allow_outside_workspace", false)},
      .execute = [request = FileReadRequest{std::move(*path_field), *options}](
                     DispatchContext& ctx) mutable { return file_read_handler(std::move(request), ctx); },
  };
}

}  // namespace

core::Result<void> register_file_read(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileReadName},
      .description = "Inspect a UTF-8 text file before answering questions about it or editing it. Reads bounded "
                     "line windows, not directories, images or PDFs. Returns a metadata header followed by exact "
                     "file text; the header is not file content. Follow its continuation offset as needed. "
                     "If the window or tool output is truncated, read a smaller relevant window before editing. "
                     "Copy the fingerprint into expected_version for a guarded edit or write.",
      .input_schema_json = std::string{kFileReadSchema},
      .required_capabilities = {core::Capability::read_file},
  };
  return registry.add_prepared(std::move(def), &prepare_file_read);
}

}  // namespace orangutan::tool
