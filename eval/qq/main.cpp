#include "../../apps/qq/chat.hpp"
#include <format>
#include <oran/bootstrap/session_status.hpp>

// Bounded live-test entry point. event.json is supplied by trusted local ingress.
#include <array>
#include <csignal>
#include <filesystem>
#include <print>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <nlohmann/json.hpp>
#include <oran/async/runtime.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/bootstrap/qq.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config/config.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/blocking.hpp>
#include <oran/io/private_directory.hpp>
#include <sodium/utils.h>

namespace {
using namespace orangutan;
using Json = nlohmann::json;
using core::Error;
constexpr std::size_t journal_max_bytes = 4 * 1024 * 1024;
using core::Result;
struct Wipe {
  std::string& value;
  ~Wipe() {
    sodium_memzero(value.data(), value.size());
  }
};
permission::RuleSet rules(bool typing = false) {
  permission::RuleSet result;
  for (const auto* operation :
       {"QQEvalRead", "QQEvalInspect", "QQEvalWrite", "QQToken", "ChannelReceive", "ChannelSend", "ChannelAttachment"})
    result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = operation});
  if (typing)
    result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelTyping"});
  return result;
}
async::Awaitable<Result<void>> authorize(hook::Bus& hooks, std::string operation, core::Capability capability) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const std::array capabilities{capability};
  auto decision = permission::evaluate(rules(), operation, capabilities, permission::Mode::strict);
  co_await hooks.publish_advisory(
      hook::Event::channel_action,
      hook::ChannelActionPayload{"qq", "live-eval", operation, decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return std::unexpected(Error::permission_denied("QQ evaluation denied"));
  co_return Result<void>{};
}
async::Awaitable<Result<void>>
save(io::PrivateDirectory& directory, const Json& journal, hook::Bus& hooks, asio::any_io_executor worker) {
  auto allowed = co_await authorize(hooks, "QQEvalWrite", core::Capability::write_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto bytes = journal.dump(2);
  if (bytes.size() > journal_max_bytes)
    co_return std::unexpected(Error::invalid_argument("QQ journal byte limit reached"));
  co_return co_await io::run_blocking(worker, [&directory, &bytes](std::stop_token) {
    return directory.write("live-journal.json", bytes);
  });
}
async::Awaitable<Result<void>> run(io::PrivateDirectory& directory,
                                   const std::string& workspace,
                                   bootstrap::RuntimeAssembly& assembly,
                                   config::Config& config,
                                   bootstrap::HttpProviderBackend& provider,
                                   http::Client& http,
                                   asio::any_io_executor worker,
                                   bool markdown,
                                   bool typing) {
  auto& hooks = assembly.hook_bus();
  auto allowed = co_await authorize(hooks, "QQEvalRead", core::Capability::read_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto read = [&](std::string name, std::size_t limit) -> async::Awaitable<Result<std::optional<std::string>>> {
    co_return co_await io::run_blocking(worker, [&directory, &name, limit](std::stop_token) {
      return directory.read(name, limit);
    });
  };
  auto credentials = co_await read("qq-credentials.json", 16384);
  if (!credentials || !*credentials)
    co_return std::unexpected(Error::not_found("QQ binding missing"));
  Wipe bytes_wipe{**credentials};
  auto binding = Json::parse(**credentials);
  auto app = binding.at("app_id").get<std::string>();
  auto owner = binding.at("user_openid").get<std::string>();
  auto secret = binding.at("app_secret").get<std::string>();
  Wipe secret_wipe{secret};
  Wipe binding_wipe{binding.at("app_secret").get_ref<std::string&>()};
  if (owner.empty() || owner.size() > 256)
    co_return std::unexpected(Error::permission_denied("scanning user identity required"));
  auto input = co_await read("event.json", 1024 * 1024);
  if (!input || !*input)
    co_return std::unexpected(Error::not_found("private event.json missing"));
  auto decoded = channel::qq().decode(**input, {app, {}});
  if (!decoded || !*decoded)
    co_return std::unexpected(Error::invalid_argument("unsupported QQ event"));
  auto message = std::move(**decoded);
  if (message.conversation.kind != channel::ChatKind::direct || message.sender != owner ||
      message.conversation.chat != owner || !message.conversation.thread.empty())
    co_return std::unexpected(Error::permission_denied("only scanning user's private messages are admitted"));
  auto previous = co_await read("live-journal.json", journal_max_bytes);
  if (!previous)
    co_return std::unexpected(previous.error());
  Json journal;
  core::TurnId identity{};
  if (*previous) {
    journal = Json::parse(**previous);
    if (journal.at("app") != app || journal.at("owner") != owner || journal.at("workspace") != workspace ||
        !journal.at("pending").is_null() || !journal.at("delivered").is_array())
      co_return std::unexpected(Error{core::ErrorKind::conflict, "binding changed or pending delivery unresolved"});
    auto parsed = core::parse_turn_id_hex(journal.at("session").get<std::string>());
    if (!parsed || core::is_zero_turn_id(*parsed))
      co_return std::unexpected(Error::parsing("invalid saved session"));
    identity = *parsed;
    for (const auto& delivered : journal.at("delivered")) {
      if (delivered.at("id") == message.event_id) {
        std::println("QQ duplicate already delivered");
        co_return Result<void>{};
      }
    }
  } else {
    auto generated = core::generate_turn_id();
    if (!generated)
      co_return std::unexpected(generated.error());
    identity = *generated;
    journal = {{"app", app},
               {"owner", owner},
               {"workspace", workspace},
               {"session", core::format_turn_id_hex(identity)},
               {"pending", nullptr},
               {"delivered", Json::array()}};
  }
  message = qq_chat::resolve_reference(std::move(message), journal.at("delivered"));
  auto token = bootstrap::QQTokenSource::create(
      {.app_id = app,
       .app_secret = std::move(secret),
       .executor = co_await asio::this_coro::executor,
       .send = [&http](http::BodyRequest request) { return http.send(std::move(request)); },
       .hooks = &hooks,
       .rules = rules()});
  if (!token)
    co_return std::unexpected(token.error());
  auto outbound = bootstrap::channel_http_transport(http, [&](channel::Conversation conversation) {
    return (*token)->get(std::move(conversation));
  });
  bootstrap::AgentSessionOptions options;
  options.executor = co_await asio::this_coro::executor;
  options.blocking_executor = worker;
  options.assembly = &assembly;
  options.config = &config;
  options.provider = &provider.system();
  options.route = provider.route();
  options.session_id = identity;
  options.scope_key = channel::conversation_key(message.conversation);
  options.agent_key = "qq-live-eval";
  options.identity = owner;
  options.origin = "qq";
  options.max_child_runs = 0;
  options.per_agent_overlay =
      "You are responding in a private QQ chat. Reply in the user's language. "
      "Attached images are supplied as native image blocks. Inspect them directly; no image-reading tool is required. "
      "Describe visible content and state uncertainty about unclear details. "
      "Base capability statements on the content actually supplied, not earlier assistant claims. "
      "Use saved memories only when relevant to the current request; do not recite unrelated notes.";
  auto session = bootstrap::AgentSession::create(std::move(options));
  if (!session)
    co_return std::unexpected(session.error());
  auto dispatcher = channel::Dispatcher::create(
      channel::qq(),
      [&](channel::Conversation conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
        const auto body = Json::parse(request.body);
        const auto type = body.value("msg_type", -1);
        const bool sending = type == 0 || type == 2;
        if (sending) {
          journal["pending"]["send_inflight"] = true;
          auto written = co_await save(directory, journal, hooks, worker);
          if (!written)
            co_return std::unexpected(written.error());
        }
        auto response = co_await outbound(std::move(conversation), std::move(request));
        if (sending && response) {
          auto receipt = channel::qq().accept(*response);
          if (receipt && !receipt->id.empty()) {
            journal["pending"]["receipts"].push_back(
                {{"id", receipt->id},
                 {"reference_key", receipt->reference_key},
                 {"sender", app},
                 {"text", type == 2 ? body.at("markdown").at("content") : body.at("content")}});
            journal["pending"]["send_inflight"] = false;
            auto written = co_await save(directory, journal, hooks, worker);
            if (!written)
              co_return std::unexpected(written.error());
          }
        }
        co_return response;
      },
      [&](channel::Message incoming) -> async::Awaitable<Result<std::string>> {
        journal["pending"] = {{"id", incoming.event_id},
                              {"text", incoming.text},
                              {"answer", nullptr},
                              {"send_inflight", false},
                              {"receipts", Json::array()},
                              {"message", qq_chat::saved_message(incoming)}};
        auto written = co_await save(directory, journal, hooks, worker);
        if (!written)
          co_return std::unexpected(written.error());
        std::string answer;
        switch (qq_chat::command(incoming)) {
          case qq_chat::Command::help:
          case qq_chat::Command::unknown:
            answer = qq_chat::help();
            break;
          case qq_chat::Command::status: {
            auto permitted = co_await authorize(hooks, "QQEvalInspect", core::Capability::read_file);
            if (!permitted)
              co_return std::unexpected(permitted.error());
            auto status = co_await bootstrap::inspect_session(assembly, identity, "qq-live-eval", worker);
            if (!status)
              co_return std::unexpected(status.error());
            answer = std::format("**当前会话**\n- 模型：{}\n- 已保存消息：{}\n- 会话：`{}`\n\n使用 /new 开始新会话。",
                                 provider.route().primary.model,
                                 status->saved_messages ? std::to_string(*status->saved_messages) : "不可用",
                                 core::format_turn_id_hex(identity));
            break;
          }
          case qq_chat::Command::new_session: {
            auto generated = core::generate_turn_id();
            if (!generated)
              co_return std::unexpected(generated.error());
            journal["session"] = core::format_turn_id_hex(*generated);
            answer = "已开启新会话。历史记录和长期记忆已保留，可以直接发送新的问题。";
            break;
          }
          case qq_chat::Command::none: {
            qq_chat::Download download = [&http](http::BodyRequest request) {
              return http.send(std::move(request));
            };
            auto prompt = co_await qq_chat::prepare_prompt(incoming, download, hooks, rules());
            if (!prompt) {
              if (prompt.error().kind() == core::ErrorKind::cancelled)
                co_return std::unexpected(prompt.error());
              answer = "图片暂时无法读取，请重新发送不超过 5 MiB 的 JPG、PNG、GIF 或 WebP 图片。";
              break;
            }
            auto generated = co_await (*session)->run_prompt(std::move(*prompt));
            if (!generated)
              co_return std::unexpected(generated.error());
            answer = std::move(generated->text);
            break;
          }
        }
        // Preflight before persisting/sending; give a useful answer instead of
        // stopping the host on QQ's passive-reply quota.
        auto parts = qq_chat::split_markdown(answer, 2000);
        if (!parts)
          answer = "回答超过了 QQ 单次回复的长度限制，请缩小问题范围或让我分步说明。";
        journal["pending"]["answer"] = answer;
        written = co_await save(directory, journal, hooks, worker);
        if (!written)
          co_return std::unexpected(written.error());
        co_return answer;
      },
      {.rules = rules(typing),
       .hooks = &hooks,
       .render_reply = markdown ? channel::RenderReply{channel::qq_markdown_reply} : channel::RenderReply{},
       .split_reply = qq_chat::split_markdown});
  if (!dispatcher)
    co_return std::unexpected(dispatcher.error());
  auto result = co_await (*dispatcher)->handle(message);
  if (!result)
    co_return std::unexpected(result.error());
  journal["pending"]["activity_failures"] = result->activity_failures;
  journal["delivered"].push_back(journal["pending"]);
  journal["pending"] = nullptr;
  auto written = co_await save(directory, journal, hooks, worker);
  if (!written)
    co_return std::unexpected(written.error());
  std::println("QQ delivered: {} parts; persisted turns: {}", result->parts_sent, journal["delivered"].size());
  co_return Result<void>{};
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4 || argc > 6) {
    std::println("eval-qq CONFIG WORKSPACE PRIVATE_STATE [--plain-text] [--no-typing]\n"
                 "Process private event.json from trusted authenticated ingress, using qq-credentials.json.\n"
                 "Only scanning user's private messages; a bounded journal retains every delivery.");
    return argc == 2 && std::string_view{argv[1]} == "--help" ? 0 : 2;
  }
  bool markdown = true, typing = true;
  for (int i = 4; i < argc; ++i) {
    const std::string_view option{argv[i]};
    if (option == "--plain-text")
      markdown = false;
    else if (option == "--no-typing")
      typing = false;
    else
      return 2;
  }
  const auto workspace = std::filesystem::canonical(argv[2]).string();
  const auto state = std::filesystem::canonical(argv[3]);
  const auto relative = state.lexically_relative(workspace);
  if (relative.empty() || *relative.begin() != "..")
    return 2;
  auto directory = io::PrivateDirectory::open(state.string());
  if (!directory)
    return 2;
  auto lock = directory->lock("login.lock");
  if (!lock)
    return 2;
  auto config = config::Config::load_file(argv[1], {.strict_unknown_fields = true});
  if (!config || !config->permissions().workspace.extra_read_roots.empty() ||
      !config->permissions().workspace.extra_write_roots.empty())
    return 2;
  async::Runtime runtime{{.io_workers = 1, .cpu_workers = static_cast<std::size_t>(config->runtime().workers)}};
  auto strand = runtime.make_strand();
  http::Client http{runtime.cpu_executor()};
  auto provider = bootstrap::HttpProviderBackend::build(
      *config,
      {.blocking_executor = runtime.cpu_executor(),
       .request_timeout = std::chrono::milliseconds{config->runtime().request_timeout_ms},
       .max_stream_bytes = static_cast<std::uint64_t>(config->runtime().stream.max_bytes)});
  if (!provider)
    return 2;
  auto assembly =
      bootstrap::RuntimeAssembly::build(workspace,
                                        runtime.cpu_executor(),
                                        {.audit_db_path = (state / "live-audit.db").string(),
                                         .trace_enabled = config->trace().enabled,
                                         .hook_blocking_timeout = std::chrono::milliseconds{config->hooks().timeout_ms},
                                         .sessions_db_path = (state / "live-sessions.db").string(),
                                         .longterm_memory_db_path = (state / "live-memory.db").string()});
  if (!assembly)
    return 2;
  asio::cancellation_signal cancellation;
  asio::signal_set signals{strand, SIGINT, SIGTERM};
  signals.async_wait([&](const asio::error_code& error, int) {
    if (!error)
      cancellation.emit(asio::cancellation_type::all);
  });
  int code = 1;
  asio::co_spawn(
      strand,
      run(*directory, workspace, *assembly, *config, *provider, http, runtime.cpu_executor(), markdown, typing),
      asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr exception, Result<void> result) {
        if (!exception && result)
          code = 0;
        else
          std::println(stderr, "QQ evaluation stopped; inspect private live-journal.json before retrying");
        signals.cancel();
        runtime.stop();
      }));
  return runtime.run() ? code : 1;
} catch (...) {
  std::println(stderr, "QQ evaluation setup failed");
  return 2;
}
