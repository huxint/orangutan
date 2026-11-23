#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace core = orangutan::core;
namespace perm = orangutan::permission;
namespace tool = orangutan::tool;
namespace test = orangutan::tests;

namespace {

struct TempWorkspace {
  std::filesystem::path dir;
  TempWorkspace() {
    auto id = core::generate_turn_id();
    REQUIRE(id.has_value());
    dir = std::filesystem::temp_directory_path() / ("oran-personal-tools-" + core::format_turn_id_hex(*id));
    std::filesystem::create_directories(dir / "notes");
    {
      std::ofstream{dir / "README.md"} << "# Orangutan Personal Notes\nHello personal agent!\n";
      std::ofstream{dir / "notes" / "todo.md"} << "- Buy coffee\n- Review Orangutan roadmap\n";
    }
  }
  ~TempWorkspace() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

}  // namespace

TEST_CASE("ShellExec requires authorization in default mode and executes shell workflows when allowed",
          "[unit][tool][shell_exec]") {
  test::run_async([](asio::io_context& io) -> orangutan::async::Awaitable<void> {
    TempWorkspace tmp;
    auto workspace = tool::Workspace::create(tmp.dir.string());
    REQUIRE(workspace.has_value());

    tool::Registry registry;
    REQUIRE(tool::register_shell_exec(registry).has_value());

    // Default mode asks for spawn_subprocess; without approval consumer it fails closed.
    const auto default_rs = perm::default_rules(perm::Mode::default_);
    perm::RecordingAuditSink audit;
    auto denied_ctx =
        tool::DispatchContext::for_now(io.get_executor(), default_rs, audit, "personal", "oran", "owner");
    denied_ctx.workspace = &*workspace;

    auto denied = co_await registry.dispatch(tool::kShellExecName, R"({"command":"ls -la"})", denied_ctx);
    REQUIRE_FALSE(denied.has_value());
    REQUIRE(denied.error().kind() == core::ErrorKind::permission_denied);

    // Explicit allow lets the agent compose shell commands (listing, searching, inspecting) inside the workspace.
    perm::RuleSet allow_rs;
    allow_rs.push_back(perm::Rule{
        .verdict = perm::Verdict::allow,
        .tool_pattern = "ShellExec",
        .capability = core::Capability::spawn_subprocess,
    });
    auto allowed_ctx =
        tool::DispatchContext::for_now(io.get_executor(), allow_rs, audit, "personal", "oran", "owner");
    allowed_ctx.workspace = &*workspace;

    auto listed = co_await registry.dispatch(tool::kShellExecName, R"({"command":"ls notes && grep -n roadmap notes/todo.md"})",
                                             allowed_ctx);
    REQUIRE(listed.has_value());
    REQUIRE_FALSE(listed->is_error);
    REQUIRE(listed->text.contains("todo.md"));
    REQUIRE(listed->text.contains("Review Orangutan roadmap"));
    REQUIRE(listed->data_json.has_value());
    const auto parsed = nlohmann::json::parse(*listed->data_json);
    REQUIRE(parsed["exit_code"] == 0);

    // Workspace-escaping working_dir is rejected before process launch.
    auto escaped = co_await registry.dispatch(tool::kShellExecName, R"({"command":"pwd","working_dir":"../"})",
                                              allowed_ctx);
    REQUIRE_FALSE(escaped.has_value());
    REQUIRE(escaped.error().kind() == core::ErrorKind::permission_denied);
  });
}
