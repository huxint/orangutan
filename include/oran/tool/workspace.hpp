// include/oran/tool/workspace.hpp — workspace path resolver for file tools.
//
// `Workspace` is the tool-layer policy object that turns LLM-authored path
// strings into canonical filesystem paths before a built-in calls `oran-io`.
// The public header deliberately stays `<filesystem>`-free; callers receive
// UTF-8 strings while the implementation owns the heavy path operations.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/io/directory_authority.hpp>

namespace orangutan::tool {

struct WorkspaceOptions {
  std::vector<std::string> extra_read_roots{};
  std::vector<std::string> extra_write_roots{};
};

struct WriteIntent {
  bool create_parent_directories{false};
};

/// Which extra-root list widens a `Workspace::lock_key` match: read covers
/// the read intents (shared locks), write covers the mutating intents
/// (exclusive locks).
enum class LockDirection {
  read,
  write,
};

struct ResolvedPath {
  /// Stable directory capability selected for this path.
  io::DirectoryAuthority authority;
  /// Path interpreted relative to `authority` for filesystem execution. This
  /// remains populated for per-call outside overrides even though the audit
  /// `relative_path` below is intentionally empty.
  std::string authority_relative_path;
  /// Canonical absolute spelling retained for audit/display. Read intents
  /// normalise symlink-ful spellings here (the pathname pass) so anchored
  /// execution receives a symlink-free relative; when the root pathname no
  /// longer names the pinned directory the spelling degrades to the
  /// workspace-lexical candidate. It is not the execution authority —
  /// callers execute through `authority` + `authority_relative_path`.
  std::string absolute_path;
  /// Path relative to the matching root, for audit/display metadata.
  /// Empty when a per-call read override resolves outside the permitted
  /// read roots; audit uses the explicit display path for that case.
  std::string relative_path;
  /// True when an existing symlink was encountered during resolution.
  bool symlink_followed{false};
  /// True when `resolve_write` accepted a missing parent because the caller
  /// explicitly allowed parent creation.
  bool created_parents{false};
  /// True when resolution used an explicit escape from the primary workspace
  /// root: either a configured extra root or a per-call read override.
  bool outside_workspace_explicit_override{false};
  /// True when a read call explicitly requested one-off outside-workspace
  /// access. Mutating resolves never set this flag.
  bool per_call_outside_workspace_override{false};
  /// Zero-based index inside the matching extra-root list; `nullopt` for the
  /// primary workspace root.
  std::optional<std::size_t> override_root_index{};
};

class Workspace {
public:
  [[nodiscard]] static core::Result<Workspace> create(std::string_view root, WorkspaceOptions options = {});

  [[nodiscard]] std::string_view root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::span<const std::string> extra_read_roots() const noexcept {
    return std::span<const std::string>{extra_read_roots_};
  }
  [[nodiscard]] std::span<const std::string> extra_write_roots() const noexcept {
    return std::span<const std::string>{extra_write_roots_};
  }

  [[nodiscard]] core::Result<ResolvedPath> resolve_read(std::string_view path) const;
  [[nodiscard]] core::Result<ResolvedPath> resolve_read_outside_workspace(std::string_view path) const;
  [[nodiscard]] core::Result<ResolvedPath> resolve_write(std::string_view path, WriteIntent intent) const;

  /// Dispatch lock-key derivation: the lexically-normalised absolute
  /// spelling of `path` joined against the workspace root, provided it falls
  /// beneath the root or a configured extra root for the requested
  /// direction. Pure string computation — no filesystem access — so the key
  /// is a deterministic function of the input and configuration rather than
  /// a racy resolution snapshot. `std::nullopt` means the path cannot name a
  /// lockable workspace target; callers skip lock serialisation and the
  /// dispatch-time resolver stays the sole access authority.
  [[nodiscard]] std::optional<std::string> lock_key(std::string_view path, LockDirection direction) const;

  /// Render an absolute path as a stable workspace display name when possible,
  /// e.g. `<workspace>/src/main.cpp`. Paths outside known roots pass through.
  [[nodiscard]] std::string display_path(std::string_view absolute_path) const;

private:
  std::string root_;
  std::vector<std::string> extra_read_roots_;
  std::vector<std::string> extra_write_roots_;
  io::DirectoryAuthority root_authority_;
  std::vector<io::DirectoryAuthority> extra_read_authorities_;
  std::vector<io::DirectoryAuthority> extra_write_authorities_;

  Workspace(std::string root,
            std::vector<std::string> extra_read_roots,
            std::vector<std::string> extra_write_roots,
            io::DirectoryAuthority root_authority,
            std::vector<io::DirectoryAuthority> extra_read_authorities,
            std::vector<io::DirectoryAuthority> extra_write_authorities);
};

}  // namespace orangutan::tool
