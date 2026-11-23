#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook.hpp>
#include <oran/io/private_directory.hpp>
#include <oran/memory.hpp>
#include <oran/storage/pool.hpp>

#include "local_provider.hpp"
#include "reminders.hpp"
#include "server.hpp"

namespace {

using namespace orangutan;

struct CliOptions {
  std::string command{"help"};
  std::string prompt;
  std::string config_path;
  std::string workspace_path{"."};
  std::string state_dir;
  std::uint16_t port{18790};
  bool demo{false};
};

void print_usage() {
  std::println(
      "Orangutan Personal Agent (C++26)\n"
      "\n"
      "Usage:\n"
      "  orangutan serve [options]       Start local loopback HTTP/SSE Personal Agent server for apps/desktop\n"
      "  orangutan chat  [options]       Interactive terminal Personal Agent session\n"
      "  orangutan ask   <prompt> [opts] Run a single Personal Agent turn and exit\n"
      "\n"
      "Options:\n"
      "  --config <file>     Provider/permission JSON config (see config.example.json)\n"
      "  --workspace <dir>   Workspace root directory (default: current directory)\n"
      "  --state <dir>       Private state directory for SQLite stores (default: <workspace>/.orangutan)\n"
      "  --port <num>        Loopback port for `serve` (default: 18790)\n"
      "  --demo              Use built-in zero-credential local demo provider (real tools & SQLite)\n"
      "  --help, -h          Show this help");
}

[[nodiscard]] core::Result<CliOptions> parse_args(int argc, char** argv) {
  CliOptions opts;
  if (argc < 2) {
    return opts;
  }
  opts.command = argv[1];
  if (opts.command == "-h" || opts.command == "--help") {
    opts.command = "help";
    return opts;
  }

  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      opts.command = "help";
      return opts;
    }
    if (arg == "--demo") {
      opts.demo = true;
    } else if (arg == "--config" && i + 1 < argc) {
      opts.config_path = argv[++i];
    } else if (arg == "--workspace" && i + 1 < argc) {
      opts.workspace_path = argv[++i];
    } else if (arg == "--state" && i + 1 < argc) {
      opts.state_dir = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      try {
        const auto parsed = std::stoul(argv[++i]);
        if (parsed == 0 || parsed > 65535) {
          return std::unexpected(core::Error::invalid_argument("--port must be between 1 and 65535"));
        }
        opts.port = static_cast<std::uint16_t>(parsed);
      } catch (...) {
        return std::unexpected(core::Error::invalid_argument("invalid --port value"));
      }
    } else if (opts.command == "ask" && !arg.starts_with("--") && opts.prompt.empty()) {
      opts.prompt = std::string{arg};
    } else {
      return std::unexpected(core::Error::invalid_argument("unknown argument").with("arg", std::string{arg}));
    }
  }
  return opts;
}

[[nodiscard]] std::string read_stdin_line() {
  std::string line;
  char ch = 0;
  while (true) {
    const int c = std::fgetc(stdin);
    if (c == EOF || c == '\n') {
      break;
    }
    ch = static_cast<char>(c);
    if (ch != '\r') {
      line.push_back(ch);
    }
  }
  return line;
}

struct TerminalSink final : provider::EventSink {
  void on_text_delta(std::string_view delta) override {
    std::print("{}", delta);
    std::fflush(stdout);
  }
  void on_tool_start(std::string_view, std::string_view name) override {
    std::println("\n[Oran · 工具] {}", name);
  }
};

}  // namespace

int main(int argc, char** argv) {
  auto parsed = parse_args(argc, argv);
  if (!parsed) {
    std::println(stderr, "Error: {}", parsed.error().message());
    return 1;
  }
  auto opts = std::move(*parsed);
  if (opts.command == "help") {
    print_usage();
    return 0;
  }
  if (opts.command != "serve" && opts.command != "chat" && opts.command != "ask") {
    std::println(stderr, "Unknown command '{}'. Run `orangutan --help`.", opts.command);
    return 1;
  }
  if (opts.command == "ask" && opts.prompt.empty()) {
    std::println(stderr, "Error: `orangutan ask` requires a prompt string.");
    return 1;
  }

  std::error_code fs_ec;
  const auto workspace_abs = std::filesystem::weakly_canonical(opts.workspace_path, fs_ec);
  if (fs_ec || !std::filesystem::is_directory(workspace_abs)) {
    std::println(stderr, "Error: invalid workspace directory '{}'", opts.workspace_path);
    return 1;
  }
  const auto state_dir =
      opts.state_dir.empty() ? (workspace_abs / ".orangutan") : std::filesystem::path{opts.state_dir};
  std::filesystem::create_directories(state_dir, fs_ec);

  config::Config cfg;
  if (!opts.config_path.empty()) {
    auto loaded = config::Config::load_file(opts.config_path);
    if (!loaded) {
      std::println(stderr, "Error loading config: {}", loaded.error().message());
      return 1;
    }
    cfg = std::move(*loaded);
  } else {
    auto loaded = config::Config::parse(R"({"runtime":{"workers":4,"prompt":{"active_tools":"defaults"}}})");
    if (!loaded) {
      std::println(stderr, "Error initializing default config: {}", loaded.error().message());
      return 1;
    }
    cfg = std::move(*loaded);
    opts.demo = true;
  }

  async::Runtime runtime{async::RuntimeConfig{
      .io_workers = 1,
      .cpu_workers = static_cast<std::size_t>(std::max<std::int64_t>(2, cfg.runtime().workers)),
  }};
  auto strand = runtime.make_strand();

  auto assembly = bootstrap::RuntimeAssembly::build(
      workspace_abs.string(),
      runtime.cpu_executor(),
      bootstrap::RuntimeAssemblyOptions{
          .audit_db_path = (state_dir / "audit.db").string(),
          .sessions_db_path = (state_dir / "sessions.db").string(),
          .longterm_memory_db_path = (state_dir / "memory.db").string(),
      });
  if (!assembly) {
    std::println(stderr, "Error initializing runtime assembly: {}", assembly.error().message());
    return 1;
  }

  auto personal_pool = storage::Pool::open(runtime.cpu_executor(),
                                           storage::PoolOptions{
                                               .path = (state_dir / "personal.db").string(),
                                               .reader_count = 1,
                                               .statement_cache_capacity = 8,
                                           });
  if (!personal_pool) {
    std::println(stderr, "Error opening personal.db: {}", personal_pool.error().message());
    return 1;
  }
  personal::ReminderStore reminders{*personal_pool};

  std::optional<bootstrap::HttpProviderBackend> http_backend;
  personal::LocalDemoProvider demo_provider;
  provider::System* provider_ptr = nullptr;
  provider::Route route;
  if (opts.demo) {
    provider_ptr = &demo_provider;
    route.primary.profile = "local-demo";
    route.primary.model = "orangutan-local-demo";
  } else {
    auto built = bootstrap::HttpProviderBackend::build(
        cfg,
        bootstrap::HttpProviderBackendOptions{
            .blocking_executor = runtime.cpu_executor(),
            .request_timeout = std::chrono::milliseconds{cfg.runtime().request_timeout_ms},
            .max_stream_bytes = static_cast<std::uint64_t>(cfg.runtime().stream.max_bytes),
        });
    if (!built) {
      std::println(stderr, "Error constructing provider backend: {}", built.error().message());
      return 1;
    }
    http_backend.emplace(std::move(*built));
    provider_ptr = &http_backend->system();
    route = http_backend->route();
  }

  int exit_code = 0;

  if (opts.command == "serve") {
    asio::co_spawn(
        strand,
        [&]() -> async::Awaitable<void> {
          auto migrated = co_await asio::co_spawn(runtime.cpu_executor(), reminders.migrate(), asio::use_awaitable);
          if (!migrated) {
            std::println(stderr, "Error migrating personal.db: {}", migrated.error().message());
            exit_code = 1;
            runtime.stop();
            co_return;
          }

          auto server = personal::PersonalServer::create(personal::PersonalServerOptions{
              .strand = strand,
              .blocking_executor = runtime.cpu_executor(),
              .assembly = &*assembly,
              .config = &cfg,
              .provider = provider_ptr,
              .route = route,
              .reminders = &reminders,
              .workspace_path = workspace_abs.string(),
              .state_dir = state_dir.string(),
              .bind_host = "127.0.0.1",
              .port = opts.port,
              .demo_mode = opts.demo,
          });
          if (!server) {
            std::println(stderr, "Error starting personal server: {}", server.error().message());
            exit_code = 1;
            runtime.stop();
            co_return;
          }

          asio::signal_set signals{strand, SIGINT, SIGTERM};
          signals.async_wait([&](std::error_code, int) { (*server)->stop(); });

          std::println("Orangutan Personal Agent listening on http://127.0.0.1:{} ({} mode)",
                       (*server)->bound_port(),
                       opts.demo ? "demo" : "live");
          std::println("Workspace: {}", workspace_abs.string());
          std::println("State:     {}", state_dir.string());
          std::fflush(stdout);

          auto ran = co_await (*server)->run();
          if (!ran) {
            std::println(stderr, "Server error: {}", ran.error().message());
            exit_code = 1;
          }
          runtime.stop();
        },
        asio::detached);

    if (auto r = runtime.run(); !r) {
      std::println(stderr, "Runtime error: {}", r.error().message());
      return 1;
    }
    return exit_code;
  }

  // Interactive terminal approval consumer for `chat` and `ask`.
  assembly->hook_bus().subscribe(
      hook::Sink{
          .id = "terminal-approval",
          .observe = {},
          .decide = [](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
            const auto* ask = std::get_if<hook::PermissionAskRenderedPayload>(payload.get());
            if (ask == nullptr) {
              co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
            }
            std::println("\n[Pebble · 权限确认] 工具: {} | 原因: {}", ask->tool_name, ask->decision_reason);
            std::println("  参数: {}", ask->input_json);
            std::print("  允许这次操作吗？[y/N]: ");
            std::fflush(stdout);
            const auto answer = read_stdin_line();
            if (answer == "y" || answer == "Y" || answer == "yes") {
              co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
            }
            co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "declined_in_terminal"};
          },
          .trusted_local = true,
      },
      {hook::Event::permission_ask_rendered});

  TerminalSink terminal_sink;
  asio::co_spawn(
      strand,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await asio::co_spawn(runtime.cpu_executor(), reminders.migrate(), asio::use_awaitable);
        if (!migrated) {
          std::println(stderr, "Error migrating personal.db: {}", migrated.error().message());
          exit_code = 1;
          runtime.stop();
          co_return;
        }

        auto make_session = [&]() {
          return bootstrap::AgentSession::create(bootstrap::AgentSessionOptions{
              .executor = strand,
              .blocking_executor = runtime.cpu_executor(),
              .assembly = &*assembly,
              .config = &cfg,
              .provider = provider_ptr,
              .route = route,
              .scope_key = "personal:owner",
              .agent_key = "oran",
              .identity = "owner",
              .origin = "cli",
              .stream = true,
              .event_sink = &terminal_sink,
          });
        };

        auto session = make_session();
        if (!session) {
          std::println(stderr, "Error creating session: {}", session.error().message());
          exit_code = 1;
          runtime.stop();
          co_return;
        }

        if (opts.command == "ask") {
          auto res = co_await (*session)->run_prompt(agent::PromptRequest{.prompt = opts.prompt});
          std::println("");
          if (!res) {
            std::println(stderr, "Error: {}", res.error().message());
            exit_code = 1;
          }
          runtime.stop();
          co_return;
        }

        std::println("Orangutan Personal Agent ({} mode) — 输入内容开始对话，/help 查看命令，/quit 退出。",
                     opts.demo ? "demo" : "live");
        while (true) {
          std::print("\n你 > ");
          std::fflush(stdout);
          if (std::feof(stdin)) {
            break;
          }
          const auto line = read_stdin_line();
          if (line.empty()) {
            continue;
          }
          if (line == "/quit" || line == "/exit") {
            break;
          }
          if (line == "/help") {
            std::println("命令: /new 新建会话 | /reminders 查看提醒 | /remind <内容> 添加30分钟后提醒 | /quit 退出");
            continue;
          }
          if (line == "/new") {
            auto fresh = make_session();
            if (fresh) {
              session = std::move(fresh);
              std::println("[Oran] 已开启新会话。");
            }
            continue;
          }
          if (line == "/reminders") {
            auto list = co_await asio::co_spawn(runtime.cpu_executor(), reminders.list(), asio::use_awaitable);
            if (!list || list->empty()) {
              std::println("[Pip] 暂时没有提醒事项。");
            } else {
              for (const auto& item : *list) {
                std::println("  [{}] {}", item.completed ? "x" : " ", item.title);
              }
            }
            continue;
          }
          if (line.starts_with("/remind ")) {
            const auto title = line.substr(8);
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            auto id = core::generate_turn_id();
            personal::Reminder rem{
                .id = id ? core::format_turn_id_hex(*id) : std::to_string(now),
                .title = title,
                .due_at_ms = now + 30 * 60 * 1000,
                .created_at_ms = now,
            };
            auto added = co_await asio::co_spawn(runtime.cpu_executor(), reminders.add(rem), asio::use_awaitable);
            if (added) {
              std::println("[Pip] 已记录提醒：{}", title);
            } else {
              std::println("[Pip] 保存提醒失败。");
            }
            continue;
          }

          std::print("Oran > ");
          std::fflush(stdout);
          auto res = co_await (*session)->run_prompt(agent::PromptRequest{.prompt = line});
          std::println("");
          if (!res) {
            std::println(stderr, "[Oran · 错误] {}", res.error().message());
          }
        }
        runtime.stop();
      },
      asio::detached);

  if (auto r = runtime.run(); !r) {
    std::println(stderr, "Runtime error: {}", r.error().message());
    return 1;
  }
  return exit_code;
}
