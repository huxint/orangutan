#include <oran/tool/builtins.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
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

constexpr std::string_view kFileEditSchema =
    R"({"type":"object","properties":{)"
    R"("path":{"type":"string","minLength":1,"description":"Absolute or workspace-relative file path."},)"
    R"("old_string":{"type":"string","minLength":1,"description":"Exact file text copied from a read, including whitespace, without the metadata header. Include enough context to select the intended occurrence."},)"
    R"("new_string":{"type":"string","description":"Replacement text; empty deletes the match. Must differ from old_string."},)"
    R"("replace_all":{"type":"boolean","default":false,"description":"False requires exactly one match; true replaces every non-overlapping match in this file."},)"
    R"("max_bytes":{"type":"integer","minimum":1,"maximum":16777216,"default":16777216,"description":"UTF-8 byte cap for both the complete source and complete edited file."},)"
    R"("expected_version":{"type":"string","description":"Opaque fingerprint copied from FileRead of this file. On conflict, read again and revise the edit; do not just drop the guard."}},)"
    R"("required":["path","old_string","new_string"],"additionalProperties":false})";

struct FileEditRequest {
  std::string old_string;
  std::string new_string;
  bool replace_all;
  std::uintmax_t max_bytes;
  std::optional<std::string> expected_version;
};

/// Indexes of every non-overlapping occurrence of `needle` in `haystack`, in
/// order. Non-overlapping is the natural fit for "replace": chained matches in
/// the input string get rewritten consistently with what a left-to-right scan
/// produces.
[[nodiscard]] std::vector<std::size_t> find_occurrences(std::string_view haystack, std::string_view needle) {
  std::vector<std::size_t> positions;
  for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    positions.push_back(pos);
  }
  return positions;
}

[[nodiscard]] core::Result<std::size_t>
replacement_size(std::size_t source_size, std::size_t old_size, std::size_t new_size, std::size_t replacement_count) {
  if (new_size <= old_size) {
    return source_size - ((old_size - new_size) * replacement_count);
  }

  const auto delta = new_size - old_size;
  const auto available = std::numeric_limits<std::size_t>::max() - source_size;
  if (replacement_count > available / delta) {
    return std::unexpected(core::Error::invalid_argument("FileEdit: replacement output is too large"));
  }
  return source_size + (delta * replacement_count);
}

/// Rebuilds the contents by stitching the unchanged slices around each match
/// with `new_string`. Returning a fresh `std::string` rather than mutating in
/// place keeps the substitution constant-time per match and avoids the
/// degenerate behaviour of repeated `string::replace` on overlapping
/// positions.
[[nodiscard]] std::string apply_replacements(std::string_view source,
                                             std::string_view old_string,
                                             std::string_view new_string,
                                             const std::vector<std::size_t>& positions) {
  std::string out;
  out.reserve(source.size() +
              (new_string.size() > old_string.size() ? (new_string.size() - old_string.size()) * positions.size() : 0));
  std::size_t cursor = 0;
  for (const auto pos : positions) {
    out.append(source.data() + cursor, pos - cursor);
    out.append(new_string);
    cursor = pos + old_string.size();
  }
  out.append(source.data() + cursor, source.size() - cursor);
  return out;
}

[[nodiscard]] async::Awaitable<core::Result<Output>> file_edit_handler(FileEditRequest request,
                                                                      DispatchContext& ctx) {
  auto& [old_string, new_string, replace_all, max_bytes, expected_version] = request;
  if (!ctx.resolved_path.has_value() || !ctx.resolved_path->authority.has_value()) {
    co_return std::unexpected(core::Error::internal("FileEdit requires a resolved workspace authority"));
  }
  const auto& path = ctx.resolved_path->absolute_path;
  auto mutation = ctx.resolved_path->authority->begin_file_mutation(ctx.resolved_path->authority_relative_path);
  if (!mutation) {
    co_return std::unexpected(std::move(mutation).error());
  }
  auto authorized_file = mutation->open_existing();
  if (!authorized_file) {
    co_return std::unexpected(std::move(authorized_file).error());
  }

  // Pre-edit fingerprint check: a stale `expected_version` aborts before
  // the read so the caller never observes a partial edit. The downstream
  // read still re-fingerprints internally for mid-read race detection;
  // this guard is the *intentional* freshness contract the agent asked for.
  if (expected_version) {
    auto pre = io::compute_file_fingerprint(*authorized_file);
    if (!pre) {
      co_return std::unexpected(core::Error{core::ErrorKind::conflict, "FileEdit: expected_version cannot be verified"}
                                    .with("path", path)
                                    .with("reason", "stale_fingerprint")
                                    .with("detail", std::string{pre.error().message()}));
    }
    const auto current_token = detail::version_token(path, *pre);
    if (*expected_version != current_token) {
      co_return std::unexpected(
          core::Error{core::ErrorKind::conflict, "FileEdit: file has changed since the expected version"}
              .with("path", path)
              .with("reason", "stale_fingerprint")
              .with("expected", *expected_version)
              .with("fingerprint", current_token));
    }
  }

  auto read = co_await io::read_text_file_ranged(ctx.executor,
                                                 std::move(*authorized_file),
                                                 io::ReadTextOptions{.max_bytes = max_bytes});
  if (!read) {
    co_return std::unexpected(std::move(read).error());
  }
  if (read->truncated) {
    co_return std::unexpected(core::Error::invalid_argument("file exceeds max_bytes")
                                 .with("path", path)
                                 .with("max_bytes", std::to_string(max_bytes)));
  }
  const auto& contents = read->text;

  const auto positions = find_occurrences(contents, old_string);
  if (positions.empty()) {
    co_return std::unexpected(
        core::Error::not_found("FileEdit: `old_string` does not occur in the file").with("path", path));
  }
  if (positions.size() > 1 && !replace_all) {
    co_return std::unexpected(
        core::Error{core::ErrorKind::conflict,
                    "FileEdit: `old_string` is not unique; pass `replace_all` to apply to every match"}
            .with("path", path)
            .with("match_count", std::to_string(positions.size())));
  }

  const auto applied = positions.size();
  auto output_size = replacement_size(contents.size(), old_string.size(), new_string.size(), positions.size());
  if (!output_size) {
    co_return std::unexpected(std::move(output_size).error().with("path", path));
  }
  if (static_cast<std::uintmax_t>(*output_size) > max_bytes) {
    co_return std::unexpected(core::Error::invalid_argument("FileEdit: output exceeds max_bytes")
                                  .with("path", path)
                                  .with("output_bytes", std::to_string(*output_size))
                                  .with("max_bytes", std::to_string(max_bytes)));
  }

  auto replaced = apply_replacements(contents, old_string, new_string, positions);
  const auto replaced_bytes = replaced.size();
  io::WriteTextOptions write_opts{.mode = io::WriteMode::truncate, .atomic = true};
  if (expected_version) {
    // Re-verify the token in the commit critical section so a file changed
    // after the pre-edit check cannot be clobbered by the replacement.
    write_opts.verify_before_commit = detail::expected_version_verifier(path, *expected_version);
  }
  auto written =
      co_await io::write_text_file(ctx.executor, std::move(*mutation), std::move(replaced), std::move(write_opts));
  if (!written) {
    co_return std::unexpected(std::move(written).error());
  }

  co_return Output{
      .text = std::format("edited {}: {} replacement{}", path, applied, applied == 1U ? "" : "s"),
      .usage =
          ToolUsage{
              .bytes_read = contents.size(),
              .bytes_written = replaced_bytes,
              .files_touched = 1,
              .match_count = applied,
          },
  };
}

[[nodiscard]] core::Result<PreparedCall> prepare_file_edit(std::string_view input_json) {
  constexpr auto fields = std::to_array<std::string_view>({
      "path", "old_string", "new_string", "replace_all", "max_bytes", "expected_version"});
  auto parsed = detail::parse_input_object(input_json, kFileEditName, fields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto path_field = detail::require_path_field(*parsed, kFileEditName);
  if (!path_field) {
    return std::unexpected(std::move(path_field).error());
  }
  auto old_string_field = detail::require_string_field(*parsed, kFileEditName, "old_string");
  if (!old_string_field) {
    return std::unexpected(std::move(old_string_field).error());
  }
  auto new_string_field = detail::require_string_field(*parsed, kFileEditName, "new_string");
  if (!new_string_field) {
    return std::unexpected(std::move(new_string_field).error());
  }

  bool replace_all = false;
  if (parsed->contains("replace_all")) {
    if (!(*parsed)["replace_all"].is_boolean()) {
      return std::unexpected(core::Error::invalid_argument("FileEdit: `replace_all` must be a boolean"));
    }
    replace_all = (*parsed)["replace_all"].get<bool>();
  }

  std::optional<std::string> expected_version;
  if (parsed->contains("expected_version")) {
    if (!(*parsed)["expected_version"].is_string()) {
      return std::unexpected(core::Error::invalid_argument("FileEdit: `expected_version` must be a string"));
    }
    expected_version = (*parsed)["expected_version"].get<std::string>();
  }

  auto max_bytes = detail::parse_file_max_bytes(*parsed, kFileEditName);
  if (!max_bytes) {
    return std::unexpected(std::move(max_bytes).error());
  }

  auto old_string = *std::move(old_string_field);
  auto new_string = *std::move(new_string_field);

  if (old_string.empty()) {
    return std::unexpected(core::Error::invalid_argument("FileEdit: `old_string` must be non-empty"));
  }
  if (old_string == new_string) {
    return std::unexpected(core::Error::invalid_argument("FileEdit: `old_string` and `new_string` are identical"));
  }

  return PreparedCall{
      .path = PathRequest{.path = *path_field, .intent = PathIntent::write},
      .execute = [request = FileEditRequest{std::move(old_string), std::move(new_string),
                                          replace_all, *max_bytes, std::move(expected_version)}](
                     DispatchContext& ctx) mutable { return file_edit_handler(std::move(request), ctx); },
  };
}

}  // namespace

core::Result<void> register_file_edit(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kFileEditName},
      .description = "Make a targeted exact string replacement in an existing UTF-8 file; this is not a regex or "
                     "patch tool. Read the relevant text first and preserve whitespace. old_string must match "
                     "exactly once unless replace_all is true. On missing or ambiguous matches, read again and "
                     "include more context instead of widening the replacement blindly. Copy expected_version "
                     "from FileRead to guard against stale content; re-read on conflict. Returns the replacement "
                     "count.",
      .input_schema_json = std::string{kFileEditSchema},
      .required_capabilities = {core::Capability::edit_file},
  };
  return registry.add_prepared(std::move(def), &prepare_file_edit);
}

}  // namespace orangutan::tool
