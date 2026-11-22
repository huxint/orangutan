#include "commands.hpp"
#include "host.hpp"
#include "input.hpp"
#include "presentation.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <print>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <oran/async/runtime.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/background_tasks.hpp>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/session_status.hpp>
#include <oran/config/config.hpp>
#include <oran/hook/bus.hpp>
#include <oran/http/client.hpp>

namespace {
using namespace orangutan;
void usage() {
  std::println("oran-telegram --config PATH --allow-user NUMERIC_ID --workspace DIRECTORY --state DIRECTORY\n"
               "  [--once] [--token-env NAME]\n"
               "oran-telegram --probe --state DIRECTORY [--token-env NAME]\n"
               "oran-telegram --ack-pending UPDATE_ID --state DIRECTORY\n"
               "Token environment defaults to ORAN_TELEGRAM_TOKEN. Provider keys use config references.\n"
               "Only the allowed user's private messages are processed. State must be outside the workspace.\n"
               "SIGINT/SIGTERM joins active work; ambiguous pending deliveries block restart.");
}
bool nested(const std::filesystem::path& parent, const std::filesystem::path& child) {
  auto relative = child.lexically_relative(parent);
  return !relative.empty() && *relative.begin() != "..";
}
}  // namespace

int main(int argc, char** argv) try {
  telegram_host::Options options;
  std::string config_path, workspace, state_path, token_env = "ORAN_TELEGRAM_TOKEN";
  std::optional<std::int64_t> acknowledge;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    if (argument == "--help") {
      usage();
      return 0;
    }
    if (argument == "--probe") {
      options.probe = true;
      continue;
    }
    if (argument == "--once") {
      options.once = true;
      continue;
    }
    if (++i >= argc) {
      usage();
      return 2;
    }
    const std::string_view value{argv[i]};
    if (argument == "--config")
      config_path = value;
    else if (argument == "--workspace")
      workspace = value;
    else if (argument == "--state")
      state_path = value;
    else if (argument == "--allow-user")
      options.user = value;
    else if (argument == "--token-env")
      token_env = value;
    else if (argument == "--ack-pending") {
      std::int64_t update{};
      const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), update);
      if (error != std::errc{} || last != value.data() + value.size() || update < 0)
        return 2;
      acknowledge = update;
    } else {
      usage();
      return 2;
    }
  }
  if (state_path.empty()) {
    usage();
    return 2;
  }
  if (!options.probe && !acknowledge) {
    std::int64_t user_id{};
    const auto [end, error] = std::from_chars(options.user.data(), options.user.data() + options.user.size(), user_id);
    if (config_path.empty() || workspace.empty() || error != std::errc{} ||
        end != options.user.data() + options.user.size() || user_id <= 0 || options.user != std::to_string(user_id)) {
      usage();
      return 2;
    }
  }
  if (acknowledge) {
    auto directory = io::PrivateDirectory::open(std::filesystem::weakly_canonical(state_path).string());
    if (!directory)
      return 2;
    auto lock = directory->lock("host.lock");
    if (!lock)
      return 2;
    auto result = telegram_host::acknowledge_pending(*directory, *acknowledge);
    std::println("{}", result ? "Pending update archived and acknowledged" : "Cannot acknowledge pending update");
    return result ? 0 : 2;
  }
  const char* value = std::getenv(token_env.c_str());
  const std::string token = value ? value : "";
  if (token.empty() || !std::ranges::all_of(token, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '_' ||
               c == '-';
      })) {
    std::println(stderr, "Telegram token environment is missing or invalid");
    return 2;
  }
  state_path = std::filesystem::weakly_canonical(state_path).string();
  if (!options.probe) {
    workspace = std::filesystem::canonical(workspace).string();
    if (nested(workspace, state_path)) {
      std::println(stderr, "State must be outside the agent workspace");
      return 2;
    }
  }
  auto directory = io::PrivateDirectory::open(state_path);
  if (!directory) {
    std::println(stderr, "Cannot open private Telegram state directory");
    return 2;
  }
  auto lock = directory->lock("host.lock");
  if (!lock) {
    std::println(stderr, "Telegram state is already in use");
    return 2;
  }
  auto state = options.probe ? core::Result<telegram_host::State>{telegram_host::State{}}
                             : telegram_host::load_state(*directory, options.user, workspace);
  if (!state) {
    std::println(stderr, "State invalid, binding changed, or pending delivery unresolved; inspect state.json");
    return 2;
  }
  std::optional<config::Config> config;
  if (!options.probe) {
    auto loaded = config::Config::load_file(config_path, {.strict_unknown_fields = true});
    if (!loaded) {
      std::println(stderr, "Cannot load provider configuration ({})", core::enum_name(loaded.error().kind()));
      return 2;
    }
    config.emplace(std::move(*loaded));
  }
  async::Runtime runtime{
      {.io_workers = 1, .cpu_workers = config ? static_cast<std::size_t>(config->runtime().workers) : 2}};
  auto strand = runtime.make_strand();
  http::Client http{runtime.cpu_executor()};
  hook::Bus probe_hooks;
  std::optional<bootstrap::HttpProviderBackend> provider;
  std::optional<bootstrap::RuntimeAssembly> assembly;
  std::unique_ptr<bootstrap::BackgroundTasks> tasks;
  std::unique_ptr<bootstrap::AgentSession> session;
  core::TurnId active_session{};
  if (config) {
    auto built = bootstrap::HttpProviderBackend::build(
        *config,
        {.blocking_executor = runtime.cpu_executor(),
         .request_timeout = std::chrono::milliseconds{config->runtime().request_timeout_ms},
         .max_stream_bytes = static_cast<std::uint64_t>(config->runtime().stream.max_bytes)});
    if (!built) {
      std::println(stderr, "Cannot construct provider ({})", core::enum_name(built.error().kind()));
      return 2;
    }
    provider.emplace(std::move(*built));
    bootstrap::RuntimeAssemblyOptions settings;
    settings.audit_db_path = state_path + "/audit.db";
    settings.sessions_db_path = state_path + "/sessions.db";
    settings.longterm_memory_db_path = state_path + "/memory.db";
    settings.trace_enabled = config->trace().enabled;
    settings.hook_blocking_timeout = std::chrono::milliseconds{config->hooks().timeout_ms};
    settings.workspace_options = {config->permissions().workspace.extra_read_roots,
                                  config->permissions().workspace.extra_write_roots};
    for (const auto& root : settings.workspace_options.extra_read_roots) {
      if (nested(std::filesystem::canonical(root), state_path)) {
        std::println(stderr, "Extra read roots must exclude Telegram state");
        return 2;
      }
    }
    for (const auto& root : settings.workspace_options.extra_write_roots) {
      if (nested(std::filesystem::canonical(root), state_path)) {
        std::println(stderr, "Extra write roots must exclude Telegram state");
        return 2;
      }
    }
    auto resources = bootstrap::RuntimeAssembly::build(workspace, runtime.cpu_executor(), std::move(settings));
    if (!resources) {
      std::println(stderr, "Cannot construct runtime ({})", core::enum_name(resources.error().kind()));
      return 2;
    }
    assembly.emplace(std::move(*resources));
  }
  if (config && !options.once && config->runtime().workers > 1) {
    // Each provider HTTP call occupies a blocking worker. Leave one worker for
    // foreground requests and channel polling instead of filling the pool.
    const auto running = std::min<std::size_t>(4, static_cast<std::size_t>(config->runtime().workers) - 1);
    auto created =
        bootstrap::BackgroundTasks::create(strand,
                                           *assembly,
                                           *config,
                                           {.max_running = running, .automatic_delivery = true, .enable_teams = true});
    if (!created) {
      std::println(stderr, "Cannot construct background task service");
      return 2;
    }
    tasks = std::move(*created);
  }
  auto& hooks = assembly ? assembly->hook_bus() : probe_hooks;
  telegram_host::Api api = [&](telegram_host::PollMethod method,
                               std::string payload) -> async::Awaitable<core::Result<channel::Response>> {
    http::BodyRequest body;
    body.method = "POST";
    body.url = "https://api.telegram.org/bot" + token + "/" + std::string{core::enum_name(method)};
    body.body = std::move(payload);
    body.headers = {{"Content-Type", "application/json"}};
    body.timeout = std::chrono::seconds{35};
    body.max_bytes = 1024 * 1024;
    auto response = co_await http.send(std::move(body));
    if (!response)
      co_return std::unexpected(core::Error{response.error().kind(), "Telegram polling HTTP failed"});
    co_return channel::Response{response->status_code, std::move(response->body), {}};
  };
  auto outbound = bootstrap::channel_http_transport(
      http,
      [&](channel::Conversation) -> async::Awaitable<core::Result<std::string>> { co_return token; });
  telegram_host::DownloadImage download = [&](std::string path) -> async::Awaitable<core::Result<std::string>> {
    http::BodyRequest request;
    request.url = "https://api.telegram.org/file/bot" + token + "/" + path;
    request.timeout = std::chrono::seconds{20};
    request.max_bytes = telegram_host::image_max_bytes;
    auto response = co_await http.send(std::move(request));
    if (!response)
      co_return std::unexpected(core::Error{response.error().kind(), "image download failed"});
    if (response->status_code != 200)
      co_return std::unexpected(core::Error::network("image download rejected"));
    co_return std::move(response->body);
  };
  permission::RuleSet image_rules;
  image_rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelAttachment"});
  permission::RuleSet presentation_rules;
  for (const auto* operation : {"ChannelStatus", "ChannelDraft"})
    presentation_rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = operation});
  auto presentation =
      std::make_shared<telegram_host::Presentation>(strand, outbound, hooks, std::move(presentation_rules));
  hooks.subscribe({.id = "telegram-presentation",
                   .observe = [presentation, owner_identity = options.user](hook::Event event, hook::PayloadPtr payload)
                       -> async::Awaitable<void> {
                     const bool parent = std::visit(
                         [&owner_identity](const auto& value) {
                           if constexpr (requires { value.who.agent_key; })
                             return value.who.agent_key == "telegram" && value.who.identity == owner_identity;
                           else
                             return false;
                         },
                         *payload);
                     if (parent)
                       presentation->observe(event);
                     co_return;
                   }},
                  {hook::Event::provider_request, hook::Event::tool_before, hook::Event::tool_after});
  auto owner = [&](core::TurnId id) {
    channel::Conversation conversation{channel::Platform::telegram,
                                       state->bot,
                                       channel::ChatKind::direct,
                                       options.user,
                                       {}};
    return bootstrap::TaskOwner{id, channel::conversation_key(conversation), "telegram", options.user};
  };
  auto ensure_session = [&](core::TurnId id) -> core::Result<bootstrap::AgentSession*> {
    if (active_session != id) {
      session.reset();
      active_session = id;
    }
    if (!session) {
      bootstrap::AgentSessionOptions settings;
      settings.executor = strand;
      settings.blocking_executor = runtime.cpu_executor();
      settings.assembly = &*assembly;
      settings.config = &*config;
      settings.provider = &provider->system();
      settings.event_sink = presentation.get();
      settings.route = provider->route();
      settings.session_id = id;
      settings.scope_key = owner(id).scope_key;
      settings.agent_key = "telegram";
      settings.identity = options.user;
      settings.origin = "telegram";
      settings.max_child_runs = 4;
      settings.background_tasks = tasks.get();
      settings.per_agent_overlay = "You are responding in a private Telegram chat. Reply in the user's language.";
      auto created = bootstrap::AgentSession::create(std::move(settings));
      if (!created)
        return std::unexpected(created.error());
      session = std::move(*created);
    }
    return session.get();
  };
  channel::RunTurn turn =
      [&, image_transport = outbound](channel::Message message) -> async::Awaitable<core::Result<std::string>> {
    auto prompt = co_await telegram_host::prepare_prompt(message, image_transport, download, hooks, image_rules);
    if (!prompt) {
      if (prompt.error().kind() == core::ErrorKind::cancelled)
        co_return std::unexpected(prompt.error());
      co_return "已收到消息，但其中或引用的图片读取失败。请重新发送不超过 5 MiB 的 JPG、PNG、GIF 或 WebP 图片。";
    }
    auto current = ensure_session(state->session);
    if (!current)
      co_return std::unexpected(current.error());
    auto result = co_await (*current)->run_prompt(std::move(*prompt));
    if (!result)
      co_return std::unexpected(result.error());
    co_return std::move(result->text);
  };
  telegram_host::BackgroundPort background;
  if (tasks) {
    background.active = [&](core::TurnId id) {
      return std::ranges::any_of(tasks->list(owner(id)), [](const auto& task) {
        return task.state == bootstrap::TaskState::queued || task.state == bootstrap::TaskState::running ||
               task.state == bootstrap::TaskState::cancelling || !task.acknowledged;
      });
    };
    background.next = [&](core::TurnId id) {
      return tasks->next_completion(owner(id));
    };
    background.complete = [&](core::TurnId id, std::string task) -> async::Awaitable<core::Result<std::string>> {
      auto current = ensure_session(id);
      if (!current)
        co_return std::unexpected(current.error());
      auto result = co_await (*current)->run_completion({.task_id = std::move(task)});
      if (!result)
        co_return std::unexpected(result.error());
      if (!*result)
        co_return std::unexpected(core::Error::internal("completion was already handled"));
      co_return std::move((**result).text);
    };
    background.control = [&](core::TurnId id, telegram_host::TaskCommand command) -> core::Result<std::string> {
      const auto key = owner(id);
      if (command == telegram_host::TaskCommand::stop) {
        tasks->cancel_owner(key);
        return "已请求停止当前会话的后台任务。清理结束前会显示正在取消；用 /tasks 查看状态。";
      }
      const auto rows = tasks->list(key);
      if (rows.empty())
        return "当前会话没有保留的后台任务。";
      std::string text = "后台任务：\n";
      for (const auto& task : rows) {
        std::string_view status;
        switch (task.state) {
          case bootstrap::TaskState::queued:
            status = "排队中";
            break;
          case bootstrap::TaskState::running:
            status = "运行中";
            break;
          case bootstrap::TaskState::cancelling:
            status = "正在取消";
            break;
          case bootstrap::TaskState::succeeded:
            status = "已完成";
            break;
          case bootstrap::TaskState::failed:
            status = "失败";
            break;
          case bootstrap::TaskState::cancelled:
            status = "已取消";
            break;
        }
        const bool finished = task.state == bootstrap::TaskState::succeeded ||
                              task.state == bootstrap::TaskState::failed ||
                              task.state == bootstrap::TaskState::cancelled;
        const auto end = finished ? task.updated_at : core::time::now_utc();
        const auto seconds =
            std::max<std::int64_t>(0,
                                   std::chrono::duration_cast<std::chrono::seconds>(
                                       end.to_system_time_point() - task.created_at.to_system_time_point())
                                       .count());
        text += std::format("- {}：{}（{} 秒）\n", task.label, status, seconds);
      }
      return text;
    };
  }
  asio::cancellation_signal cancellation;
  asio::signal_set signals{strand, SIGINT, SIGTERM};
  bool stopping = false;
  signals.async_wait([&](const asio::error_code& error, int) {
    if (!error) {
      stopping = true;
      cancellation.emit(asio::cancellation_type::all);
    }
  });
  int exit_code = 1;
  asio::co_spawn(
      strand,
      [&]() -> async::Awaitable<core::Result<void>> {
        core::Result<void> result = std::unexpected(core::Error::internal("Telegram host failed"));
        bool threw = false;
        try {
          result = co_await telegram_host::run(
              options,
              std::move(api),
              std::move(outbound),
              std::move(turn),
              hooks,
              *directory,
              *state,
              runtime.cpu_executor(),
              presentation.get(),
              [&](core::TurnId id) -> async::Awaitable<core::Result<std::string>> {
                auto status = co_await bootstrap::inspect_session(*assembly, id, "telegram", runtime.cpu_executor());
                if (!status)
                  co_return std::unexpected(status.error());
                co_return telegram_host::format_session_status(*status, provider->route().primary.model);
              },
              std::move(background));
        } catch (...) {
          threw = true;
        }
        if (threw) {
          const auto cancellation = co_await asio::this_coro::cancellation_state;
          result = std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                       ? core::Error::cancelled()
                                       : core::Error::internal("Telegram host failed"));
        }
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        co_await asio::this_coro::throw_if_cancelled(false);
        if (tasks) {
          auto drained = co_await tasks->shutdown();
          if (!drained)
            co_return std::unexpected(drained.error());
        }
        co_return result;
      },
      asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr exception, core::Result<void> result) {
        exit_code = !exception && (result || (stopping && result.error().kind() == core::ErrorKind::cancelled)) ? 0 : 1;
        if (exception || !result)
          std::println(stderr,
                       "Telegram host stopped ({}); inspect state.json before restarting",
                       exception ? "cancelled_or_internal" : core::enum_name(result.error().kind()));
        signals.cancel();
        runtime.stop();
      }));
  auto ran = runtime.run();
  return ran ? exit_code : 1;
} catch (const std::exception&) {
  std::println(stderr, "Telegram host setup failed");
  return 2;
}
