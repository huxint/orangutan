// src/oran-tool/workspace.cpp — workspace path policy implementation.

#include <oran/tool/workspace.hpp>


#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::tool {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

struct RootMatch {
  std::filesystem::path root;
  std::optional<std::size_t> override_index{};
};

[[nodiscard]] Error path_error(std::string message, std::string_view input, std::string_view reason) {
  return Error::permission_denied(std::move(message))
      .with("path", std::string{input})
      .with("reason", std::string{reason});
}

[[nodiscard]] Error
filesystem_error(std::string message, const std::filesystem::path& path, const std::error_code& ec) {
  return Error::io(std::move(message)).with("path", path.string()).with("detail", ec.message());
}

[[nodiscard]] Result<std::filesystem::path> canonical_directory(std::string_view raw, std::string_view context_key) {
  if (raw.empty()) {
    return std::unexpected(
        Error::invalid_argument("workspace root must not be empty").with("field", std::string{context_key}));
  }

  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::path{std::string{raw}}, ec);
  if (ec) {
    return std::unexpected(
        filesystem_error("failed to canonicalize workspace root", std::filesystem::path{std::string{raw}}, ec)
            .with("field", std::string{context_key}));
  }
  canonical = canonical.lexically_normal();

  const auto status = std::filesystem::status(canonical, ec);
  // libstdc++ sets `ec = ENOENT` even though `status.type()` already
  // reports `file_not_found`; pass ENOENT through to the existence check
  // below so the caller gets `Error::not_found`, not `Error::io`.
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return std::unexpected(
        filesystem_error("failed to inspect workspace root", canonical, ec).with("field", std::string{context_key}));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("workspace root does not exist")
                               .with("path", canonical.string())
                               .with("field", std::string{context_key}));
  }
  if (!std::filesystem::is_directory(status)) {
    return std::unexpected(Error::invalid_argument("workspace root must be a directory")
                               .with("path", canonical.string())
                               .with("field", std::string{context_key}));
  }
  return canonical;
}

[[nodiscard]] Result<std::vector<std::string>> canonicalize_extra_roots(std::span<const std::string> roots,
                                                                        std::string_view field_name) {
  std::vector<std::string> out;
  out.reserve(roots.size());
  for (std::size_t index = 0; index < roots.size(); ++index) {
    auto field = std::format("{}[{}]", field_name, index);
    auto canonical = canonical_directory(roots[index], field);
    if (!canonical) {
      return std::unexpected(std::move(canonical).error());
    }
    out.push_back(canonical->string());
  }
  return out;
}

[[nodiscard]] Result<std::vector<io::DirectoryAuthority>> open_root_authorities(std::span<const std::string> roots,
                                                                                std::string_view field_name) {
  std::vector<io::DirectoryAuthority> out;
  out.reserve(roots.size());
  for (std::size_t index = 0; index < roots.size(); ++index) {
    auto authority = io::DirectoryAuthority::open_trusted(roots[index]);
    if (!authority) {
      return std::unexpected(std::move(authority).error().with("field", std::format("{}[{}]", field_name, index)));
    }
    out.push_back(std::move(*authority));
  }
  return out;
}

[[nodiscard]] bool is_under_root(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  const auto normalized_candidate = candidate.lexically_normal();
  const auto normalized_root = root.lexically_normal();

  auto cand_it = normalized_candidate.begin();
  auto root_it = normalized_root.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++cand_it) {
    if (cand_it == normalized_candidate.end() || *cand_it != *root_it) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::filesystem::path join_input(const std::filesystem::path& root, std::string_view input) {
  auto raw = std::filesystem::path{std::string{input}};
  if (raw.is_absolute()) {
    return raw.lexically_normal();
  }
  return (root / raw).lexically_normal();
}

[[nodiscard]] bool has_symlink_component(const std::filesystem::path& candidate) {
  std::filesystem::path current;
  for (const auto& part : candidate.lexically_normal()) {
    current /= part;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(current, ec);
    if (ec || !std::filesystem::exists(status)) {
      continue;
    }
    if (std::filesystem::is_symlink(status)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::optional<RootMatch> find_matching_root(const std::filesystem::path& candidate,
                                                          const std::filesystem::path& workspace_root,
                                                          std::span<const std::string> extra_roots) {
  if (is_under_root(candidate, workspace_root)) {
    return RootMatch{.root = workspace_root};
  }
  for (std::size_t index = 0; index < extra_roots.size(); ++index) {
    auto root = std::filesystem::path{extra_roots[index]};
    if (is_under_root(candidate, root)) {
      return RootMatch{.root = std::move(root), .override_index = index};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string relative_path(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  auto relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return ".";
  }
  return relative.string();
}

[[nodiscard]] std::string relative_display_path(const std::filesystem::path& candidate,
                                                const std::filesystem::path& root) {
  auto relative = candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return ".";
  }
  return relative.generic_string();
}

[[nodiscard]] const io::DirectoryAuthority&
authority_for_match(const RootMatch& match,
                    const io::DirectoryAuthority& primary_authority,
                    std::span<const io::DirectoryAuthority> extra_authorities) {
  return match.override_index.has_value() ? extra_authorities[*match.override_index] : primary_authority;
}

[[nodiscard]] Result<ResolvedPath> build_resolved(const std::filesystem::path& candidate,
                                                  const RootMatch& match,
                                                  const io::DirectoryAuthority& primary_authority,
                                                  std::span<const io::DirectoryAuthority> extra_authorities,
                                                  bool symlink_followed,
                                                  bool created_parents) {
  const auto& authority = authority_for_match(match, primary_authority, extra_authorities);
  return ResolvedPath{
      .authority = authority,
      .authority_relative_path = relative_path(candidate, match.root),
      .absolute_path = candidate.lexically_normal().string(),
      .relative_path = relative_path(candidate, match.root),
      .symlink_followed = symlink_followed,
      .created_parents = created_parents,
      .outside_workspace_explicit_override = match.override_index.has_value(),
      .per_call_outside_workspace_override = false,
      .override_root_index = match.override_index,
  };
}

[[nodiscard]] Result<ResolvedPath> build_per_call_outside_resolved(const std::filesystem::path& candidate,
                                                                   bool symlink_followed) {
  auto authority_root = candidate.parent_path();
  auto relative = candidate.filename().string();
  if (authority_root.empty() || relative.empty()) {
    authority_root = candidate;
    relative = ".";
  }
  auto authority = io::DirectoryAuthority::open_trusted(authority_root.string());
  if (!authority) {
    return std::unexpected(std::move(authority).error());
  }
  return ResolvedPath{
      .authority = std::move(*authority),
      .authority_relative_path = std::move(relative),
      .absolute_path = candidate.lexically_normal().string(),
      .relative_path = {},
      .symlink_followed = symlink_followed,
      .created_parents = false,
      .outside_workspace_explicit_override = true,
      .per_call_outside_workspace_override = true,
      .override_root_index = std::nullopt,
  };
}

[[nodiscard]] Result<std::filesystem::path>
canonical_existing_path(const std::filesystem::path& candidate, std::string_view input, std::string_view action) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(candidate, ec);
  if (ec && status.type() != std::filesystem::file_type::not_found) {
    return std::unexpected(filesystem_error(std::string{"failed to inspect path for "}.append(action), candidate, ec));
  }
  if (!std::filesystem::exists(status)) {
    return std::unexpected(Error::not_found("workspace path does not exist").with("path", std::string{input}));
  }

  auto canonical = std::filesystem::weakly_canonical(candidate, ec);
  if (ec) {
    return std::unexpected(
        filesystem_error(std::string{"failed to canonicalize path for "}.append(action), candidate, ec));
  }
  return canonical.lexically_normal();
}

[[nodiscard]] Result<ResolvedPath> resolve_existing_readable(std::string_view input,
                                                             const std::string& root,
                                                             std::span<const std::string> extra_roots,
                                                             const io::DirectoryAuthority& primary_authority,
                                                             std::span<const io::DirectoryAuthority> extra_authorities,
                                                             std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);

  if (!find_matching_root(candidate, workspace_root, extra_roots).has_value()) {
    return std::unexpected(path_error("path is outside workspace", input, "outside_workspace"));
  }

  auto canonical = canonical_existing_path(candidate, input, action);
  if (!canonical) {
    return std::unexpected(std::move(canonical).error());
  }

  auto match = find_matching_root(*canonical, workspace_root, extra_roots);
  if (!match) {
    return std::unexpected(path_error(symlink_followed ? "path symlink escapes workspace" : "path is outside workspace",
                                      input,
                                      symlink_followed ? "symlink_escape" : "outside_workspace"));
  }

  return build_resolved(*canonical, *match, primary_authority, extra_authorities, symlink_followed, false);
}

[[nodiscard]] Result<ResolvedPath>
resolve_existing_readable_outside_workspace(std::string_view input, const std::string& root, std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);

  auto canonical = canonical_existing_path(candidate, input, action);
  if (!canonical) {
    return std::unexpected(std::move(canonical).error());
  }

  return build_per_call_outside_resolved(*canonical, symlink_followed);
}

[[nodiscard]] Result<ResolvedPath> resolve_mutating(std::string_view input,
                                                    const std::string& root,
                                                    std::span<const std::string> extra_roots,
                                                    const io::DirectoryAuthority& primary_authority,
                                                    std::span<const io::DirectoryAuthority> extra_authorities,
                                                    bool allow_missing_parent,
                                                    std::string_view action) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  const bool symlink_followed = has_symlink_component(candidate);
  if (symlink_followed) {
    return std::unexpected(path_error("mutating path resolves through a symlink", input, "symlink_target"));
  }

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(candidate, ec);
  if (ec && status.type() != std::filesystem::file_type::not_found) {
    return std::unexpected(filesystem_error(std::string{"failed to inspect path for "}.append(action), candidate, ec));
  }

  auto canonical = candidate.lexically_normal();
  bool created_parents = false;
  if (std::filesystem::exists(status)) {
    auto resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      return std::unexpected(
          filesystem_error(std::string{"failed to canonicalize path for "}.append(action), candidate, ec));
    }
    canonical = resolved.lexically_normal();
  } else {
    const auto parent = candidate.parent_path();
    if (!parent.empty()) {
      const auto parent_status = std::filesystem::symlink_status(parent, ec);
      if (ec && parent_status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(
            filesystem_error(std::string{"failed to inspect parent for "}.append(action), parent, ec));
      }
      const auto parent_exists = std::filesystem::exists(parent_status);
      created_parents = !parent_exists && allow_missing_parent;
      if (parent_exists) {
        auto resolved_parent = std::filesystem::weakly_canonical(parent, ec);
        if (ec) {
          return std::unexpected(
              filesystem_error(std::string{"failed to canonicalize parent for "}.append(action), parent, ec));
        }
        canonical = (resolved_parent / candidate.filename()).lexically_normal();
      }
    }
  }

  auto match = find_matching_root(canonical, workspace_root, extra_roots);
  if (!match) {
    return std::unexpected(path_error("path is outside workspace", input, "outside_workspace"));
  }

  return build_resolved(canonical, *match, primary_authority, extra_authorities, false, created_parents);
}

/// Resolve a read intent as a two-pass pipeline. The pathname pass
/// (`resolve_existing_readable`) is the symlink normaliser, not the
/// authority: it rewrites symlink-ful spellings — including absolute-target
/// symlinks, which `RESOLVE_BENEATH` cannot follow even when the target
/// stays inside the workspace — into the symlink-free canonical relative
/// that anchored execution then opens beneath the pinned root. The pinned
/// `DirectoryAuthority` remains the sole execution authority; when the
/// trusted root spelling no longer names the pinned directory, the pathname
/// pass is skipped entirely and resolution degrades to the stricter
/// anchored-probe shape (beneath-relative symlinks only).
[[nodiscard]] Result<ResolvedPath>
resolve_read_through_authority(std::string_view input,
                               const std::string& root,
                               std::span<const std::string> extra_roots,
                               const io::DirectoryAuthority& primary_authority,
                               std::span<const io::DirectoryAuthority> extra_authorities) {
  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("workspace path must not be empty"));
  }

  const auto workspace_root = std::filesystem::path{root};
  const auto candidate = join_input(workspace_root, input);
  auto match = find_matching_root(candidate, workspace_root, extra_roots);
  if (!match) {
    return std::unexpected(path_error("path is outside workspace", input, "outside_workspace"));
  }

  const auto& authority = authority_for_match(*match, primary_authority, extra_authorities);
  auto root_still_named = authority.refers_to_path(match->root.string());
  if (!root_still_named) {
    return std::unexpected(std::move(root_still_named).error());
  }

  if (*root_still_named) {
    auto canonical = resolve_existing_readable(input, root, extra_roots, primary_authority, extra_authorities, "read");
    if (canonical) {
      return canonical;
    }
  }

  const auto relative = relative_path(candidate, match->root);
  auto opened = authority.open_file(io::AnchoredPath{
      .relative_path = relative,
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  if (!opened) {
    auto error = std::move(opened).error();
    const auto reason =
        std::ranges::find_if(error.context(), [](const auto& entry) { return entry.first == "reason"; });
    if (reason != error.context().end() &&
        (reason->second == "outside_authority" || reason->second == "symlink_component")) {
      return std::unexpected(path_error("path symlink escapes workspace", input, "symlink_escape"));
    }
    return std::unexpected(std::move(error));
  }

  return build_resolved(candidate, *match, primary_authority, extra_authorities, false, false);
}

}  // namespace

Workspace::Workspace(std::string root,
                     std::vector<std::string> extra_read_roots,
                     std::vector<std::string> extra_write_roots,
                     io::DirectoryAuthority root_authority,
                     std::vector<io::DirectoryAuthority> extra_read_authorities,
                     std::vector<io::DirectoryAuthority> extra_write_authorities)
    : root_{std::move(root)}, extra_read_roots_{std::move(extra_read_roots)},
      extra_write_roots_{std::move(extra_write_roots)}, root_authority_{std::move(root_authority)},
      extra_read_authorities_{std::move(extra_read_authorities)},
      extra_write_authorities_{std::move(extra_write_authorities)} {}

core::Result<Workspace> Workspace::create(std::string_view root, WorkspaceOptions options) {
  auto canonical_root = canonical_directory(root, "workspace");
  if (!canonical_root) {
    return std::unexpected(std::move(canonical_root).error());
  }

  auto read_roots = canonicalize_extra_roots(options.extra_read_roots, "permissions.workspace.extra_read_roots");
  if (!read_roots) {
    return std::unexpected(std::move(read_roots).error());
  }
  auto write_roots = canonicalize_extra_roots(options.extra_write_roots, "permissions.workspace.extra_write_roots");
  if (!write_roots) {
    return std::unexpected(std::move(write_roots).error());
  }

  auto root_authority = io::DirectoryAuthority::open_trusted(canonical_root->string());
  if (!root_authority) {
    return std::unexpected(std::move(root_authority).error().with("field", "workspace"));
  }
  auto read_authorities = open_root_authorities(*read_roots, "permissions.workspace.extra_read_roots");
  if (!read_authorities) {
    return std::unexpected(std::move(read_authorities).error());
  }
  auto write_authorities = open_root_authorities(*write_roots, "permissions.workspace.extra_write_roots");
  if (!write_authorities) {
    return std::unexpected(std::move(write_authorities).error());
  }

  return Workspace{canonical_root->string(),
                   std::move(*read_roots),
                   std::move(*write_roots),
                   std::move(*root_authority),
                   std::move(*read_authorities),
                   std::move(*write_authorities)};
}

core::Result<ResolvedPath> Workspace::resolve_read(std::string_view path) const {
  return resolve_read_through_authority(path, root_, extra_read_roots_, root_authority_, extra_read_authorities_);
}

core::Result<ResolvedPath> Workspace::resolve_read_outside_workspace(std::string_view path) const {
  return resolve_existing_readable_outside_workspace(path, root_, "read");
}

core::Result<ResolvedPath> Workspace::resolve_write(std::string_view path, WriteIntent intent) const {
  return resolve_mutating(path,
                          root_,
                          extra_write_roots_,
                          root_authority_,
                          extra_write_authorities_,
                          intent.create_parent_directories,
                          "write");
}

std::optional<std::string> Workspace::lock_key(std::string_view path, LockDirection direction) const {
  if (path.empty()) {
    return std::nullopt;
  }
  const auto workspace_root = std::filesystem::path{root_};
  const auto candidate = join_input(workspace_root, path);
  const auto& extra_roots = direction == LockDirection::read ? extra_read_roots_ : extra_write_roots_;
  if (!find_matching_root(candidate, workspace_root, extra_roots).has_value()) {
    return std::nullopt;
  }
  return candidate.string();
}

std::string Workspace::display_path(std::string_view absolute_path) const {
  const auto candidate = std::filesystem::path{std::string{absolute_path}}.lexically_normal();
  const auto workspace_root = std::filesystem::path{root_};
  const auto render = [&](std::string_view label, const std::filesystem::path& root) {
    const auto relative = relative_display_path(candidate, root);
    if (relative == ".") {
      return std::string{label};
    }
    return std::format("{}/{}", label, relative);
  };

  if (is_under_root(candidate, workspace_root)) {
    return render("<workspace>", workspace_root);
  }
  for (std::size_t index = 0; index < extra_read_roots_.size(); ++index) {
    const auto root = std::filesystem::path{extra_read_roots_[index]};
    if (is_under_root(candidate, root)) {
      return render(std::format("<read-root-{}>", index), root);
    }
  }
  for (std::size_t index = 0; index < extra_write_roots_.size(); ++index) {
    const auto root = std::filesystem::path{extra_write_roots_[index]};
    if (is_under_root(candidate, root)) {
      return render(std::format("<write-root-{}>", index), root);
    }
  }
  return std::string{absolute_path};
}

}  // namespace orangutan::tool
