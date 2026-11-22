// tests/tool/test_workspace.cpp — workspace resolver coverage.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace io = orangutan::io;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary};
  REQUIRE(out.good());
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

[[nodiscard]] bool context_has(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

[[nodiscard]] bool is_hex_digest(std::string_view value) {
  return value.size() == 64U &&
         std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

[[nodiscard]] nlohmann::json path_resolution_metadata(const permission::AuditEvent& event) {
  auto metadata = nlohmann::json::parse(event.metadata_json);
  REQUIRE(metadata.is_object());
  REQUIRE(metadata.contains("path_resolution"));
  auto path_resolution = metadata["path_resolution"];
  REQUIRE(path_resolution.is_object());
  return path_resolution;
}

void create_symlink_or_skip(const std::filesystem::path& target, const std::filesystem::path& link) {
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec) {
    SKIP(std::string{"test filesystem does not allow symlink creation: "} + ec.message());
  }
}

[[nodiscard]] tool::Workspace make_workspace(const std::filesystem::path& root, tool::WorkspaceOptions options = {}) {
  auto workspace = tool::Workspace::create(root.string(), std::move(options));
  REQUIRE(workspace.has_value());
  return std::move(*workspace);
}

[[nodiscard]] permission::RuleSet allow_file_read_rules() {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "FileRead"});
  return rules;
}

[[nodiscard]] permission::RuleSet allow_tool_rules(std::string tool_name, core::Capability capability) {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::move(tool_name),
      .capability = capability,
  });
  return rules;
}

[[nodiscard]] permission::RuleSet ask_tool_rules(std::string tool_name, core::Capability capability) {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::ask,
      .tool_pattern = std::move(tool_name),
      .capability = capability,
      .replay_max = 1,
      .approval_ttl = std::chrono::seconds{60},
  });
  return rules;
}

[[nodiscard]] tool::DispatchContext make_workspace_ctx(asio::io_context& io,
                                                       permission::RuleSet& rules,
                                                       permission::AuditSink& sink,
                                                       tool::Workspace& workspace) {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::default_,
      .rules = rules,
      .audit = sink,
      .workspace = &workspace,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = "operator-1",
  };
}

[[nodiscard]] permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

[[nodiscard]] core::Time fixed_now() noexcept {
  using namespace std::chrono;
  return core::Time{sys_days{year{2026} / January / day{1}}};
}

[[nodiscard]] permission::ApprovalToken grant(permission::ApprovalBroker& broker,
                                              std::string_view tool_name,
                                              std::string_view input,
                                              std::string_view identity,
                                              core::Time now) {
  return broker.approve(
      permission::ApprovalGrant{
          .tool_name = tool_name,
          .input = input,
          .identity = identity,
          .ttl = std::chrono::seconds{60},
          .replay_max = 1,
      },
      now);
}

}  // namespace

TEST_CASE("Workspace resolves relative and absolute paths under the root", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-inside"};
  write_text(root.path() / "nested" / "note.txt", "hello");
  auto workspace = make_workspace(root.path());

  auto relative = workspace.resolve_read("nested/note.txt");
  REQUIRE(relative.has_value());
  REQUIRE(relative->absolute_path == (root.path() / "nested" / "note.txt").string());
  REQUIRE(relative->relative_path == "nested/note.txt");
  REQUIRE_FALSE(relative->symlink_followed);
  REQUIRE_FALSE(relative->outside_workspace_explicit_override);

  auto absolute = workspace.resolve_read((root.path() / "nested" / "note.txt").string());
  REQUIRE(absolute.has_value());
  REQUIRE(absolute->absolute_path == relative->absolute_path);
  REQUIRE(absolute->relative_path == relative->relative_path);
}

TEST_CASE("Workspace rejects traversal outside the root", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-traversal"};
  auto workspace = make_workspace(root.path());

  auto read = workspace.resolve_read("../outside.txt");
  REQUIRE_FALSE(read.has_value());
  REQUIRE(read.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(read.error(), "reason", "outside_workspace"));

  const auto deny_cases = {
      std::string{"../outside.txt"},
      std::string{"/etc/passwd"},
      std::string{"legit/../../../outside.txt"},
  };
  for (const auto& input : deny_cases) {
    auto resolved = workspace.resolve_write(input, tool::WriteIntent{});
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(resolved.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(resolved.error(), "reason", "outside_workspace"));
  }
}

TEST_CASE("Workspace follows inside read symlinks and rejects symlink escapes", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-symlink-root"};
  TempDir outside{"oran-workspace-symlink-outside"};
  write_text(root.path() / "target.txt", "inside");
  write_text(outside.path() / "secret.txt", "outside");

  create_symlink_or_skip(root.path() / "target.txt", root.path() / "inside-link.txt");
  create_symlink_or_skip(outside.path() / "secret.txt", root.path() / "outside-link.txt");

  auto workspace = make_workspace(root.path());

  auto inside = workspace.resolve_read("inside-link.txt");
  REQUIRE(inside.has_value());
  // The pathname pass normalises the symlink-ful spelling (this fixture's
  // link target is absolute, which `RESOLVE_BENEATH` alone cannot follow)
  // to the canonical target that anchored execution then opens.
  REQUIRE(inside->absolute_path == (root.path() / "target.txt").string());
  REQUIRE(inside->symlink_followed);

  auto escaped = workspace.resolve_read("outside-link.txt");
  REQUIRE_FALSE(escaped.has_value());
  REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(escaped.error(), "reason", "symlink_escape"));
}

TEST_CASE("Workspace read resolution survives a replaced root pathname", "[unit][tool][workspace]") {
  TempDir sandbox{"oran-workspace-read-root-replaced"};
  const auto root = sandbox.path() / "workspace";
  const auto moved_root = sandbox.path() / "workspace-moved";
  const auto outside = sandbox.path() / "outside";
  write_text(root / "note.txt", "inside-original");
  write_text(outside / "note.txt", "outside-target");

  auto workspace = make_workspace(root);

  std::error_code ec;
  std::filesystem::rename(root, moved_root, ec);
  REQUIRE(ec.value() == 0);
  create_symlink_or_skip(outside, root);

  // Resolution goes through the pinned root descriptor, not the pathname:
  // the swapped-in symlink at the original spelling never redirects the read.
  auto resolved = workspace.resolve_read("note.txt");
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->absolute_path == (root / "note.txt").string());
  REQUIRE(resolved->relative_path == "note.txt");
  REQUIRE_FALSE(resolved->symlink_followed);

  auto missing = workspace.resolve_read("absent.txt");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("Workspace lock_key derives deterministic keys without filesystem access", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-lock-key"};
  TempDir read_extra{"oran-workspace-lock-key-read"};
  auto workspace = make_workspace(root.path(),
                                  tool::WorkspaceOptions{
                                      .extra_read_roots = {read_extra.path().string()},
                                  });

  // Pure string derivation: a missing file still keys, and relative,
  // absolute, and dot-dot spellings of the same target agree.
  const auto expected = (root.path() / "note.txt").string();
  REQUIRE(workspace.lock_key("note.txt", tool::LockDirection::read) == expected);
  REQUIRE(workspace.lock_key(expected, tool::LockDirection::write) == expected);
  REQUIRE(workspace.lock_key("sub/../note.txt", tool::LockDirection::write) == expected);

  REQUIRE_FALSE(workspace.lock_key("../outside.txt", tool::LockDirection::read).has_value());
  REQUIRE_FALSE(workspace.lock_key("/etc/passwd", tool::LockDirection::write).has_value());
  REQUIRE_FALSE(workspace.lock_key("", tool::LockDirection::read).has_value());

  // Extra roots widen only their own direction.
  const auto extra_target = (read_extra.path() / "log.txt").string();
  REQUIRE(workspace.lock_key(extra_target, tool::LockDirection::read) == extra_target);
  REQUIRE_FALSE(workspace.lock_key(extra_target, tool::LockDirection::write).has_value());
}

TEST_CASE("Workspace refuses mutating paths that traverse symlinks", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-write-symlink"};
  write_text(root.path() / "target.txt", "inside");
  create_symlink_or_skip(root.path() / "target.txt", root.path() / "link.txt");
  auto workspace = make_workspace(root.path());

  auto write = workspace.resolve_write("link.txt", tool::WriteIntent{});
  REQUIRE_FALSE(write.has_value());
  REQUIRE(write.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write.error(), "reason", "symlink_target"));

}

TEST_CASE("Workspace extra roots widen only the configured direction", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-primary"};
  TempDir readable{"oran-workspace-readable"};
  TempDir writable{"oran-workspace-writable"};
  write_text(readable.path() / "audit.log", "audit");

  auto workspace = make_workspace(root.path(),
                                  tool::WorkspaceOptions{
                                      .extra_read_roots = {readable.path().string()},
                                      .extra_write_roots = {writable.path().string()},
                                  });

  auto read = workspace.resolve_read((readable.path() / "audit.log").string());
  REQUIRE(read.has_value());
  REQUIRE(read->outside_workspace_explicit_override);
  REQUIRE(read->override_root_index.has_value());
  REQUIRE(*read->override_root_index == 0U);
  REQUIRE(read->relative_path == "audit.log");

  auto write_to_read_root = workspace.resolve_write((readable.path() / "audit.log").string(), tool::WriteIntent{});
  REQUIRE_FALSE(write_to_read_root.has_value());
  REQUIRE(write_to_read_root.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write_to_read_root.error(), "reason", "outside_workspace"));

  auto write_to_write_root = workspace.resolve_write((writable.path() / "created.txt").string(), tool::WriteIntent{});
  REQUIRE(write_to_write_root.has_value());
  REQUIRE(write_to_write_root->outside_workspace_explicit_override);
  REQUIRE(write_to_write_root->override_root_index.has_value());
  REQUIRE(*write_to_write_root->override_root_index == 0U);
}

TEST_CASE("Workspace outside read override does not grant write access", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-per-call-primary"};
  TempDir outside{"oran-workspace-per-call-outside"};
  write_text(outside.path() / "audit.log", "audit");

  auto workspace = make_workspace(root.path());

  auto read = workspace.resolve_read_outside_workspace((outside.path() / "audit.log").string());
  REQUIRE(read.has_value());
  REQUIRE(read->absolute_path == (outside.path() / "audit.log").string());
  REQUIRE(read->relative_path.empty());
  REQUIRE(read->outside_workspace_explicit_override);
  REQUIRE(read->per_call_outside_workspace_override);
  REQUIRE_FALSE(read->override_root_index.has_value());

  auto write = workspace.resolve_write((outside.path() / "created.txt").string(), tool::WriteIntent{});
  REQUIRE_FALSE(write.has_value());
  REQUIRE(write.error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(context_has(write.error(), "reason", "outside_workspace"));
}

TEST_CASE("Workspace display_path renders stable root-relative labels", "[unit][tool][workspace]") {
  TempDir root{"oran-workspace-display"};
  TempDir readable{"oran-workspace-display-readable"};
  write_text(root.path() / "src" / "main.cpp", "int main() {}\n");
  write_text(readable.path() / "logs" / "audit.txt", "audit\n");
  auto workspace = make_workspace(root.path(),
                                  tool::WorkspaceOptions{
                                      .extra_read_roots = {readable.path().string()},
                                  });

  REQUIRE(workspace.display_path(root.path().string()) == "<workspace>");
  REQUIRE(workspace.display_path((root.path() / "src" / "main.cpp").string()) == "<workspace>/src/main.cpp");
  REQUIRE(workspace.display_path((readable.path() / "logs" / "audit.txt").string()) == "<read-root-0>/logs/audit.txt");
  REQUIRE(workspace.display_path("/tmp/oran-outside-display.txt") == "/tmp/oran-outside-display.txt");
}

TEST_CASE("Workspace instances keep independent roots", "[unit][tool][workspace]") {
  TempDir left{"oran-workspace-left"};
  TempDir right{"oran-workspace-right"};
  write_text(left.path() / "foo.txt", "left");
  write_text(right.path() / "foo.txt", "right");

  auto left_workspace = make_workspace(left.path());
  auto right_workspace = make_workspace(right.path());

  auto left_resolved = left_workspace.resolve_read("foo.txt");
  auto right_resolved = right_workspace.resolve_read("foo.txt");
  REQUIRE(left_resolved.has_value());
  REQUIRE(right_resolved.has_value());
  REQUIRE(left_resolved->absolute_path == (left.path() / "foo.txt").string());
  REQUIRE(right_resolved->absolute_path == (right.path() / "foo.txt").string());
  REQUIRE(left_resolved->absolute_path != right_resolved->absolute_path);
}

TEST_CASE("Registry pre-resolves workspace paths before permission evaluation and records audit metadata",
          "[unit][tool][workspace][audit]") {
  TempDir root{"oran-workspace-audit-deny"};
  write_text(root.path() / "note.txt", "inside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    permission::RuleSet rules;
    rules.push_back(permission::Rule{
        .verdict = permission::Verdict::deny,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });
    permission::RecordingAuditSink sink;
    auto workspace = make_workspace(root.path());
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto denied = co_await registry.dispatch(tool::kFileReadName, R"({"path":"note.txt"})", ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::deny);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"] == "note.txt");
    REQUIRE(metadata["input_path_hash"] == permission::to_hex(permission::ApprovalAuthority::input_hash("note.txt")));
    REQUIRE(metadata["workspace_root_hash"] ==
            permission::to_hex(permission::ApprovalAuthority::input_hash(workspace.root())));
    REQUIRE(metadata["symlink_followed"] == false);
    REQUIRE(metadata["created_parents"] == false);
    REQUIRE(metadata["outside_workspace_explicit_override"] == false);
    REQUIRE(metadata["override_root_index"].is_null());
  });
}

TEST_CASE("Registry audit metadata records workspace override root matches", "[unit][tool][workspace][audit]") {
  TempDir root{"oran-workspace-audit-primary"};
  TempDir readable{"oran-workspace-audit-readable"};
  write_text(readable.path() / "audit.log", "needle in readable root");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_read_roots = {readable.path().string()},
                                    });
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    const auto input = std::format(R"({{"path":"{}"}})", (readable.path() / "audit.log").string());
    auto read = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("needle in readable root"));

    REQUIRE(sink.events().size() == 1);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"] == "audit.log");
    REQUIRE(metadata["outside_workspace_explicit_override"] == true);
    REQUIRE(metadata["override_root_index"] == 0);
    REQUIRE(is_hex_digest(metadata["input_path_hash"].get<std::string>()));
    REQUIRE(is_hex_digest(metadata["workspace_root_hash"].get<std::string>()));
  });
}

TEST_CASE("Registry audits path policy failures before ask approval and does not spend replay",
          "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-audit-fail"};
  TempDir outside{"oran-workspace-audit-fail-outside"};
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_write_roots = {outside.path().string()},
                                    });
    auto rules = ask_tool_rules(std::string{tool::kFileReadName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto input = std::format(R"({{"path":"{}"}})", outside_relative_path.string());

    auto broker = make_broker();
    const auto now = fixed_now();
    const auto token = grant(broker, tool::kFileReadName, input, "operator-1", now);
    ctx.approval_broker = &broker;
    ctx.approval_token = &token;
    ctx.now = now;

    auto denied = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(denied.error(), "reason", "outside_workspace"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"].is_null());
    REQUIRE(metadata["error_kind"] == "permission_denied");
    REQUIRE(metadata["error_reason"] == "outside_workspace");
    REQUIRE(is_hex_digest(metadata["input_path_hash"].get<std::string>()));
    REQUIRE(is_hex_digest(metadata["workspace_root_hash"].get<std::string>()));

    auto still_unspent = broker.check(token, tool::kFileReadName, input, "operator-1", now);
    REQUIRE(still_unspent.has_value());
    auto now_spent = broker.check(token, tool::kFileReadName, input, "operator-1", now);
    REQUIRE_FALSE(now_spent.has_value());
    REQUIRE(context_has(now_spent.error(), "reason", "replay_exhausted"));
  });
}

TEST_CASE("Read-side outside-workspace override forces approval and records explicit audit display",
          "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-readside-override-primary"};
  TempDir outside{"oran-workspace-readside-override-outside"};
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path(),
                                    tool::WorkspaceOptions{
                                        .extra_write_roots = {outside.path().string()},
                                    });
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    const auto input =
        std::format(R"({{"path":"{}","allow_outside_workspace":true}})", (outside.path() / "secret.txt").string());
    auto denied = co_await registry.dispatch(tool::kFileReadName, input, ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(denied.error(), "reason", "approval_required"));
    REQUIRE(context_has(denied.error(), "decision_reason", "outside_workspace_override"));

    REQUIRE(sink.events().size() == 1);
    REQUIRE(sink.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(sink.events()[0].outcome == permission::AuditOutcome::ask);
    REQUIRE(sink.events()[0].reason == "outside_workspace_override");
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"].is_null());
    REQUIRE(metadata["resolved_display_path"] == (outside.path() / "secret.txt").string());
    REQUIRE(metadata["outside_workspace_explicit_override"] == true);
    REQUIRE(metadata["per_call_outside_workspace_override"] == true);
    REQUIRE(metadata["override_root_index"].is_null());
  });
}

TEST_CASE("Approved outside-workspace reads retain their audit scope", "[unit][tool][workspace][audit][approval]") {
  TempDir root{"oran-workspace-readside-approved-primary"};
  TempDir outside{"oran-workspace-readside-approved-outside"};
  write_text(outside.path() / "secret.txt", "outside needle");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    permission::RuleSet rules;
    rules.push_back(permission::Rule{
        .verdict = permission::Verdict::allow,
        .tool_pattern = std::string{tool::kFileReadName},
        .capability = core::Capability::read_file,
    });

    auto workspace = make_workspace(root.path());
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    const auto now = fixed_now();
    ctx.approval_broker = &broker;
    ctx.now = now;

    const auto read_input =
        std::format(R"({{"path":"{}","allow_outside_workspace":true}})", (outside.path() / "secret.txt").string());
    auto read_token = grant(broker, tool::kFileReadName, read_input, "operator-1", now);
    ctx.approval_token = &read_token;
    auto read = co_await registry.dispatch(tool::kFileReadName, read_input, ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("outside needle"));

    REQUIRE(sink.events().size() == 1);
    for (const auto& event : sink.events()) {
      REQUIRE(event.verdict == permission::Verdict::ask);
      REQUIRE(event.outcome == permission::AuditOutcome::approved);
      REQUIRE(event.reason == "outside_workspace_override");
      const auto metadata = path_resolution_metadata(event);
      REQUIRE(metadata["resolved_relative_path"].is_null());
      REQUIRE(metadata["outside_workspace_explicit_override"] == true);
      REQUIRE(metadata["per_call_outside_workspace_override"] == true);
      REQUIRE(metadata["override_root_index"].is_null());
    }
    REQUIRE(path_resolution_metadata(sink.events()[0])["resolved_display_path"] ==
            (outside.path() / "secret.txt").string());

  });
}

TEST_CASE("FileRead uses DispatchContext workspace when supplied", "[unit][tool][workspace][file_read]") {
  TempDir root{"oran-workspace-file-read"};
  TempDir outside{"oran-workspace-file-read-outside"};
  write_text(root.path() / "note.txt", "inside");
  write_text(outside.path() / "secret.txt", "outside");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_file_read_rules();
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto read = co_await registry.dispatch("FileRead", R"({"path":"note.txt"})", ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("\ninside"));
    REQUIRE(read->text.contains("fingerprint=v1:"));

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto outside_relative = outside_relative_path.string();
    const auto escaped_input = std::format(R"({{"path":"{}"}})", outside_relative);
    auto escaped = co_await registry.dispatch("FileRead", escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });
}

TEST_CASE("FileRead retains workspace authority across the approval window",
          "[unit][tool][workspace][file_read][approval][race]") {
  TempDir sandbox{"oran-workspace-file-read-approval-race"};
  const auto root = sandbox.path() / "workspace";
  const auto moved_root = sandbox.path() / "workspace-moved";
  const auto outside = sandbox.path() / "outside";
  write_text(root / "note.txt", "inside-original");
  write_text(outside / "note.txt", "outside-target");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root);
    auto rules = ask_tool_rules(std::string{tool::kFileReadName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    ctx.approval_broker = &broker;
    ctx.now = fixed_now();

    orangutan::hook::Bus bus;
    orangutan::hook::Sink prompt{
        .id = "root-replacement-prompt",
        .decide = [&](orangutan::hook::Event,
                      orangutan::hook::PayloadPtr) -> async::Awaitable<core::Result<orangutan::hook::HookDecision>> {
          std::error_code ec;
          std::filesystem::rename(root, moved_root, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to rename workspace root").with("detail", ec.message()));
          }
          std::filesystem::create_directory_symlink(outside, root, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to replace workspace root").with("detail", ec.message()));
          }
          co_return orangutan::hook::HookDecision{
              .reason = "operator_approved:operator-1",
              .rewritten_input_json = std::nullopt,
              .trace = {},
          };
        },
    };
    bus.subscribe(prompt, {orangutan::hook::Event::permission_ask_rendered});
    ctx.bus = &bus;

    auto read = co_await registry.dispatch(tool::kFileReadName, R"({"path":"note.txt"})", ctx);
    REQUIRE(read.has_value());
    REQUIRE(read->text.contains("\ninside-original"));
    REQUIRE_FALSE(read->text.contains("outside-target"));

    std::ifstream outside_input{outside / "note.txt", std::ios::binary};
    std::string outside_text;
    std::getline(outside_input, outside_text);
    REQUIRE(outside_text == "outside-target");

    REQUIRE(sink.events().size() == 1U);
    const auto metadata = path_resolution_metadata(sink.events()[0]);
    REQUIRE(metadata["resolved_relative_path"] == "note.txt");
    REQUIRE(metadata["resolved_display_path"] == "<workspace>/note.txt");
  });
}

TEST_CASE("FileRead rejects a symlink escape introduced during approval",
          "[unit][tool][workspace][file_read][approval][race]") {
  TempDir sandbox{"oran-workspace-file-read-symlink-race"};
  const auto root = sandbox.path() / "workspace";
  const auto moved_directory = sandbox.path() / "workspace" / "moved-safe";
  const auto outside = sandbox.path() / "outside";
  write_text(root / "safe" / "note.txt", "inside-original");
  write_text(outside / "note.txt", "outside-target");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_read(registry).has_value());

    auto workspace = make_workspace(root);
    auto rules = ask_tool_rules(std::string{tool::kFileReadName}, core::Capability::read_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    ctx.approval_broker = &broker;
    ctx.now = fixed_now();

    orangutan::hook::Bus bus;
    orangutan::hook::Sink prompt{
        .id = "symlink-race-prompt",
        .decide = [&](orangutan::hook::Event,
                      orangutan::hook::PayloadPtr) -> async::Awaitable<core::Result<orangutan::hook::HookDecision>> {
          std::error_code ec;
          std::filesystem::rename(root / "safe", moved_directory, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to rename safe directory").with("detail", ec.message()));
          }
          std::filesystem::create_directory_symlink(outside, root / "safe", ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to introduce escaping symlink").with("detail", ec.message()));
          }
          co_return orangutan::hook::HookDecision{
              .reason = "operator_approved:operator-1",
              .rewritten_input_json = std::nullopt,
              .trace = {},
          };
        },
    };
    bus.subscribe(prompt, {orangutan::hook::Event::permission_ask_rendered});
    ctx.bus = &bus;

    auto read = co_await registry.dispatch(tool::kFileReadName, R"({"path":"safe/note.txt"})", ctx);
    REQUIRE_FALSE(read.has_value());
    const auto reason =
        std::ranges::find_if(read.error().context(), [](const auto& entry) { return entry.first == "reason"; });
    REQUIRE(reason != read.error().context().end());
    REQUIRE(reason->second == "outside_authority");

    std::ifstream outside_input{outside / "note.txt", std::ios::binary};
    std::string outside_text;
    std::getline(outside_input, outside_text);
    REQUIRE(outside_text == "outside-target");
  });
}

TEST_CASE("FileWrite uses DispatchContext workspace for relative writes and traversal refusal",
          "[unit][tool][workspace][file_write]") {
  TempDir root{"oran-workspace-file-write"};
  TempDir outside{"oran-workspace-file-write-outside"};

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileWriteName}, core::Capability::write_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto written = co_await registry.dispatch(tool::kFileWriteName,
                                              R"({"path":"nested/out.txt","content":"inside","create_parents":true})",
                                              ctx);
    REQUIRE(written.has_value());
    REQUIRE(std::filesystem::exists(root.path() / "nested" / "out.txt"));

    auto appended = co_await registry.dispatch(tool::kFileWriteName,
                                               R"({"path":"nested/out.txt","content":"-tail","mode":"append"})",
                                               ctx);
    REQUIRE(appended.has_value());

    auto refused =
        co_await registry.dispatch(tool::kFileWriteName,
                                   R"({"path":"nested/out.txt","content":"clobber","mode":"fail_if_exists"})",
                                   ctx);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().kind() == core::ErrorKind::conflict);

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "blocked.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input =
        std::format(R"({{"path":"{}","content":"escape","create_parents":true}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileWriteName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
    REQUIRE_FALSE(std::filesystem::exists(outside.path() / "blocked.txt"));
  });

  std::ifstream written{root.path() / "nested" / "out.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{written}, std::istreambuf_iterator<char>{}} == "inside-tail");
}

TEST_CASE("FileWrite retains workspace authority across the approval window",
          "[unit][tool][workspace][file_write][approval][race]") {
  TempDir sandbox{"oran-workspace-file-write-approval-race"};
  const auto root = sandbox.path() / "workspace";
  const auto moved_root = sandbox.path() / "workspace-moved";
  const auto outside = sandbox.path() / "outside";
  write_text(root / "note.txt", "inside-old");
  write_text(outside / "note.txt", "outside-old");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_write(registry).has_value());

    auto workspace = make_workspace(root);
    auto rules = ask_tool_rules(std::string{tool::kFileWriteName}, core::Capability::write_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    ctx.approval_broker = &broker;
    ctx.now = fixed_now();

    orangutan::hook::Bus bus;
    orangutan::hook::Sink prompt{
        .id = "write-root-replacement-prompt",
        .decide = [&](orangutan::hook::Event,
                      orangutan::hook::PayloadPtr) -> async::Awaitable<core::Result<orangutan::hook::HookDecision>> {
          std::error_code ec;
          std::filesystem::rename(root, moved_root, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to rename workspace root").with("detail", ec.message()));
          }
          std::filesystem::create_directory_symlink(outside, root, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to replace workspace root").with("detail", ec.message()));
          }
          co_return orangutan::hook::HookDecision{
              .reason = "operator_approved:operator-1",
              .rewritten_input_json = std::nullopt,
              .trace = {},
          };
        },
    };
    bus.subscribe(prompt, {orangutan::hook::Event::permission_ask_rendered});
    ctx.bus = &bus;

    auto written =
        co_await registry.dispatch(tool::kFileWriteName, R"({"path":"note.txt","content":"inside-new"})", ctx);
    REQUIRE(written.has_value());
  });

  std::ifstream inside{moved_root / "note.txt", std::ios::binary};
  std::ifstream outside_input{outside / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "inside-new");
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_input}, std::istreambuf_iterator<char>{}} ==
          "outside-old");
}

TEST_CASE("FileEdit uses DispatchContext workspace for relative edits and traversal refusal",
          "[unit][tool][workspace][file_edit]") {
  TempDir root{"oran-workspace-file-edit"};
  TempDir outside{"oran-workspace-file-edit-outside"};
  write_text(root.path() / "note.txt", "alpha beta");
  write_text(outside.path() / "secret.txt", "do not edit");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileEditName}, core::Capability::edit_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto edited = co_await registry.dispatch(tool::kFileEditName,
                                             R"({"path":"note.txt","old_string":"beta","new_string":"BETA"})",
                                             ctx);
    REQUIRE(edited.has_value());

    std::error_code ec;
    const auto outside_relative_path = std::filesystem::relative(outside.path() / "secret.txt", root.path(), ec);
    REQUIRE(ec.value() == 0);
    const auto escaped_input =
        std::format(R"({{"path":"{}","old_string":"do","new_string":"DO"}})", outside_relative_path.string());
    auto escaped = co_await registry.dispatch(tool::kFileEditName, escaped_input, ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(escaped.error(), "reason", "outside_workspace"));
  });

  std::ifstream inside{root.path() / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "alpha BETA");
  std::ifstream outside_file{outside.path() / "secret.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_file}, std::istreambuf_iterator<char>{}} == "do not edit");
}

TEST_CASE("FileEdit rejects a symlink escape introduced during approval",
          "[unit][tool][workspace][file_edit][approval][race]") {
  TempDir sandbox{"oran-workspace-file-edit-approval-race"};
  const auto root = sandbox.path() / "workspace";
  const auto moved_safe = root / "moved-safe";
  const auto outside = sandbox.path() / "outside";
  write_text(root / "safe" / "note.txt", "inside alpha");
  write_text(outside / "note.txt", "outside alpha");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());

    auto workspace = make_workspace(root);
    auto rules = ask_tool_rules(std::string{tool::kFileEditName}, core::Capability::edit_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);
    auto broker = make_broker();
    ctx.approval_broker = &broker;
    ctx.now = fixed_now();

    orangutan::hook::Bus bus;
    orangutan::hook::Sink prompt{
        .id = "edit-symlink-race-prompt",
        .decide = [&](orangutan::hook::Event,
                      orangutan::hook::PayloadPtr) -> async::Awaitable<core::Result<orangutan::hook::HookDecision>> {
          std::error_code ec;
          std::filesystem::rename(root / "safe", moved_safe, ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to rename safe directory").with("detail", ec.message()));
          }
          std::filesystem::create_directory_symlink(outside, root / "safe", ec);
          if (ec) {
            co_return std::unexpected(
                core::Error::io("test failed to introduce escaping symlink").with("detail", ec.message()));
          }
          co_return orangutan::hook::HookDecision{
              .reason = "operator_approved:operator-1",
              .rewritten_input_json = std::nullopt,
              .trace = {},
          };
        },
    };
    bus.subscribe(prompt, {orangutan::hook::Event::permission_ask_rendered});
    ctx.bus = &bus;

    auto edited = co_await registry.dispatch(tool::kFileEditName,
                                             R"({"path":"safe/note.txt","old_string":"alpha","new_string":"ALPHA"})",
                                             ctx);
    REQUIRE_FALSE(edited.has_value());
    REQUIRE(context_has(edited.error(), "reason", "symlink_component"));
  });

  std::ifstream inside{moved_safe / "note.txt", std::ios::binary};
  std::ifstream outside_input{outside / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "inside alpha");
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_input}, std::istreambuf_iterator<char>{}} ==
          "outside alpha");
}

TEST_CASE("FileEdit rejects workspace symlink mutation targets", "[unit][tool][workspace][file_edit]") {
  TempDir root{"oran-workspace-file-edit-link"};
  write_text(root.path() / "target.txt", "alpha");
  create_symlink_or_skip(root.path() / "target.txt", root.path() / "link.txt");

  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    tool::Registry registry;
    REQUIRE(tool::register_file_edit(registry).has_value());

    auto workspace = make_workspace(root.path());
    auto rules = allow_tool_rules(std::string{tool::kFileEditName}, core::Capability::edit_file);
    permission::RecordingAuditSink sink;
    auto ctx = make_workspace_ctx(io, rules, sink, workspace);

    auto edited = co_await registry.dispatch(tool::kFileEditName,
                                             R"({"path":"link.txt","old_string":"alpha","new_string":"ALPHA"})",
                                             ctx);
    REQUIRE_FALSE(edited.has_value());
    REQUIRE(edited.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(context_has(edited.error(), "reason", "symlink_target"));
    REQUIRE(std::filesystem::is_symlink(root.path() / "link.txt"));
  });

  std::ifstream target{root.path() / "target.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{target}, std::istreambuf_iterator<char>{}} == "alpha");
}
