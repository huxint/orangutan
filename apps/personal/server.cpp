#include "server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

#include <oran/core/content.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/provider/types.hpp>
#include <oran/tool/builtins.hpp>

namespace orangutan::personal {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxHttpBodyBytes = 1024U * 1024U;
constexpr std::string_view kPersonalScopeKey = "personal:owner";
constexpr std::string_view kSharedScopeKey = "shared:workspace";
constexpr std::string_view kAgentKey = "oran";

[[nodiscard]] std::int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t to_ms(core::Time t) noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.to_system_time_point().time_since_epoch()).count();
}

[[nodiscard]] std::string make_id() {
  if (auto id = core::generate_turn_id(); id) {
    return core::format_turn_id_hex(*id);
  }
  return std::format("id-{}", now_ms());
}

[[nodiscard]] std::string companion_for_tool(std::string_view /*tool_name*/) {
  // Companions represent task-splitting teammates, not per-tool silos;
  // default foreground turns report under the lead companion `oran`.
  return "oran";
}

[[nodiscard]] std::string approval_target_summary(std::string_view tool_name, std::string_view input_json) {
  auto parsed = json::parse(input_json, nullptr, false);
  if (parsed.is_object()) {
    if (parsed.contains("path") && parsed["path"].is_string()) {
      return parsed["path"].get<std::string>();
    }
    if (parsed.contains("command") && parsed["command"].is_string()) {
      return parsed["command"].get<std::string>();
    }
  }
  return std::string{tool_name};
}

[[nodiscard]] std::string extract_message_text(const core::Message& msg) {
  std::string out;
  for (const auto& block : msg.blocks) {
    if (const auto* text = std::get_if<core::TextContent>(&block)) {
      if (!out.empty()) {
        out.push_back('\n');
      }
      out += text->text;
    }
  }
  return out;
}

[[nodiscard]] std::string http_status_text(int code) noexcept {
  switch (code) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 413:
      return "Payload Too Large";
    default:
      return "Internal Server Error";
  }
}

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string origin;
  std::string body;
};

[[nodiscard]] std::string url_decode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(input[i + 1]);
      const int lo = hex(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (input[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

[[nodiscard]] std::string cors_origin_header(std::string_view origin) {
  if (origin.starts_with("http://127.0.0.1:") || origin.starts_with("http://localhost:") ||
      origin == "http://127.0.0.1" || origin == "http://localhost") {
    return std::string{origin};
  }
  return "http://127.0.0.1:5173";
}

[[nodiscard]] std::string make_http_json(int status, const json& payload, std::string_view origin) {
  const auto body = payload.dump();
  return std::format(
      "HTTP/1.1 {} {}\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "Content-Length: {}\r\n"
      "Access-Control-Allow-Origin: {}\r\n"
      "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type\r\n"
      "Connection: close\r\n\r\n{}",
      status,
      http_status_text(status),
      body.size(),
      cors_origin_header(origin),
      body);
}

[[nodiscard]] std::string make_http_options(std::string_view origin) {
  return std::format(
      "HTTP/1.1 204 No Content\r\n"
      "Access-Control-Allow-Origin: {}\r\n"
      "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type\r\n"
      "Content-Length: 0\r\n"
      "Connection: close\r\n\r\n",
      cors_origin_header(origin));
}

struct ConversationRecord {
  std::string id;
  std::string session_id;
  std::string name;
  bool custom_name{false};
  std::int64_t created_at_ms{0};
};

struct ApprovalRecord {
  std::string id;
  std::string task_id;
  std::string conversation_id;
  std::string tool_name;
  std::string target;
  std::string status{"pending"};
  std::optional<bool> decision;
  std::shared_ptr<asio::steady_timer> gate_timer;
};

struct TaskRecord {
  std::string id;
  std::string conversation_id;
  std::string session_id;
  std::string title;
  std::string status{"running"};
  std::string active_companion{"oran"};
  std::string active_tool{};
  std::int64_t created_at_ms{0};
  std::optional<std::string> approval_id;
  std::shared_ptr<asio::cancellation_signal> cancel_signal;
};

struct SseClient {
  asio::ip::tcp::socket socket;
  std::deque<std::string> queue;
  bool writing{false};
  bool closed{false};

  explicit SseClient(asio::ip::tcp::socket s) : socket{std::move(s)} {}
};

}  // namespace

class PersonalServer::Impl : public std::enable_shared_from_this<PersonalServer::Impl> {
public:
  Impl(PersonalServerOptions opts, asio::ip::tcp::acceptor acceptor)
      : options_{std::move(opts)}, acceptor_{std::move(acceptor)},
        reminder_timer_{options_.strand} {
    load_conversations();
    if (conversations_.empty()) {
      auto id = make_id();
      conversations_.push_back(ConversationRecord{
          .id = id,
          .session_id = id,
          .name = "和 Oran 聊聊",
          .custom_name = false,
          .created_at_ms = now_ms(),
      });
      save_conversations();
    }
    wire_hooks();
  }

  [[nodiscard]] std::uint16_t bound_port() const noexcept {
    std::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
  }

  void stop() noexcept {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    std::error_code ec;
    acceptor_.close(ec);
    reminder_timer_.cancel();
    for (auto& [_, task] : tasks_) {
      if (task.cancel_signal) {
        task.cancel_signal->emit(asio::cancellation_type::terminal);
      }
    }
    for (auto& [_, approval] : approvals_) {
      if (approval.status == "pending") {
        approval.status = "cancelled";
        approval.decision = false;
        if (approval.gate_timer) {
          approval.gate_timer->cancel();
        }
      }
    }
    for (auto& client : sse_clients_) {
      client->closed = true;
      client->socket.close(ec);
    }
    sse_clients_.clear();
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> run() {
    asio::co_spawn(options_.strand, reminder_loop(), asio::detached);
    while (!stopped_) {
      auto [ec, socket] = co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
      if (ec) {
        if (stopped_ || ec == asio::error::operation_aborted) {
          break;
        }
        continue;
      }
      asio::co_spawn(options_.strand, handle_connection(std::move(socket)), asio::detached);
    }
    co_return core::Result<void>{};
  }

private:
  void wire_hooks() {
    auto& bus = options_.assembly->hook_bus();

    // Blocking interactive approval gate: pauses C++ coroutine cleanly until
    // the local user approves/rejects via `POST /api/approvals/:id`.
    bus.subscribe(
        hook::Sink{
            .id = "personal-host-approval",
            .observe = {},
            .decide = [this](hook::Event, hook::PayloadPtr payload)
                -> async::Awaitable<core::Result<hook::HookDecision>> {
              const auto* ask = std::get_if<hook::PermissionAskRenderedPayload>(payload.get());
              if (ask == nullptr) {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              if (permission_policy_ == "allow") {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              if (permission_policy_ == "deny") {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "denied_by_local_policy"};
              }

              const auto approval_id = make_id();
              const auto target = approval_target_summary(ask->tool_name, ask->input_json);
              auto timer = std::make_shared<asio::steady_timer>(options_.strand);
              timer->expires_at(std::chrono::steady_clock::time_point::max());

              std::string matched_task;
              std::string matched_conv;
              for (auto& [tid, task] : tasks_) {
                if (task.status == "running") {
                  matched_task = tid;
                  matched_conv = task.conversation_id;
                  task.status = "awaiting_approval";
                  task.active_companion = "pebble";
                  task.approval_id = approval_id;
                  break;
                }
              }

              approvals_[approval_id] = ApprovalRecord{
                  .id = approval_id,
                  .task_id = matched_task,
                  .conversation_id = matched_conv,
                  .tool_name = ask->tool_name,
                  .target = target,
                  .status = "pending",
                  .decision = std::nullopt,
                  .gate_timer = timer,
              };

              broadcast_sse("approval_requested",
                            json{
                                {"id", approval_id},
                                {"taskId", matched_task},
                                {"conversationId", matched_conv},
                                {"tool", ask->tool_name},
                                {"target", target},
                                {"reason", ask->decision_reason},
                                {"status", "pending"},
                            });

              auto [wait_ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
              static_cast<void>(wait_ec);

              const auto cancel_state = co_await asio::this_coro::cancellation_state;
              auto it = approvals_.find(approval_id);
              if (cancel_state.cancelled() != asio::cancellation_type::none) {
                if (it != approvals_.end() && it->second.status == "pending") {
                  it->second.status = "cancelled";
                  it->second.decision = false;
                }
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "cancelled"};
              }
              if (it != approvals_.end() && it->second.decision.value_or(false)) {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "declined_by_user"};
            },
            .trusted_local = true,
        },
        {hook::Event::permission_ask_rendered});

    // Advisory observer broadcasting live Forest Companion tool activity.
    bus.subscribe(
        hook::Sink{
            .id = "personal-host-companion-observer",
            .observe = [this](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<void> {
              if (event == hook::Event::tool_before) {
                if (const auto* before = std::get_if<hook::ToolBeforePayload>(payload.get())) {
                  const auto pet = companion_for_tool(before->tool_name);
                  for (auto& [_, task] : tasks_) {
                    if (task.status == "running") {
                      task.active_companion = pet;
                      task.active_tool = before->tool_name;
                    }
                  }
                  broadcast_sse("tool_activity",
                                json{{"phase", "start"}, {"tool", before->tool_name}, {"companion", pet}});
                }
              } else if (event == hook::Event::tool_after) {
                if (const auto* after = std::get_if<hook::ToolAfterPayload>(payload.get())) {
                  broadcast_sse("tool_activity",
                                json{{"phase", "end"},
                                     {"tool", after->tool_name},
                                     {"companion", companion_for_tool(after->tool_name)},
                                     {"succeeded", after->succeeded}});
                }
              } else if (event == hook::Event::memory_write_after || event == hook::Event::memory_forget) {
                broadcast_sse("memory_changed", json{{"companion", "moss"}});
              }
              co_return;
            },
            .decide = {},
            .trusted_local = false,
        },
        {hook::Event::tool_before,
         hook::Event::tool_after,
         hook::Event::memory_write_after,
         hook::Event::memory_forget});
  }

  [[nodiscard]] std::filesystem::path conversations_file() const {
    return std::filesystem::path{options_.state_dir} / "conversations.json";
  }

  void load_conversations() {
    std::ifstream in{conversations_file()};
    if (!in.is_open()) {
      return;
    }
    auto parsed = json::parse(in, nullptr, false);
    if (!parsed.is_array()) {
      return;
    }
    for (const auto& item : parsed) {
      if (!item.is_object() || !item.contains("id") || !item.contains("sessionId") || !item.contains("name")) {
        continue;
      }
      conversations_.push_back(ConversationRecord{
          .id = item["id"].get<std::string>(),
          .session_id = item["sessionId"].get<std::string>(),
          .name = item["name"].get<std::string>(),
          .custom_name = item.value("customName", false),
          .created_at_ms = item.value("createdAt", std::int64_t{0}),
      });
    }
  }

  void save_conversations() const {
    std::error_code ec;
    std::filesystem::create_directories(options_.state_dir, ec);
    json arr = json::array();
    for (const auto& c : conversations_) {
      arr.push_back(json{
          {"id", c.id},
          {"sessionId", c.session_id},
          {"name", c.name},
          {"customName", c.custom_name},
          {"createdAt", c.created_at_ms},
      });
    }
    const auto target = conversations_file();
    const auto staging = target.string() + ".tmp";
    {
      std::ofstream out{staging, std::ios::binary | std::ios::trunc};
      if (!out.is_open()) {
        return;
      }
      out << arr.dump(2);
    }
    std::filesystem::rename(staging, target, ec);
  }

  void broadcast_sse(std::string_view event_name, const json& data) {
    const auto frame = std::format("event: {}\ndata: {}\n\n", event_name, data.dump());
    std::erase_if(sse_clients_, [](const auto& c) { return c->closed; });
    for (auto& client : sse_clients_) {
      if (client->queue.size() >= 128U) {
        client->queue.pop_front();
      }
      client->queue.push_back(frame);
      if (!client->writing) {
        client->writing = true;
        asio::co_spawn(options_.strand, pump_sse(client), asio::detached);
      }
    }
  }

  [[nodiscard]] async::Awaitable<void> pump_sse(std::shared_ptr<SseClient> client) {
    while (!client->closed && !client->queue.empty()) {
      auto frame = std::move(client->queue.front());
      client->queue.pop_front();
      auto [ec, _] = co_await asio::async_write(client->socket, asio::buffer(frame), asio::as_tuple(asio::use_awaitable));
      if (ec) {
        client->closed = true;
        std::error_code close_ec;
        client->socket.close(close_ec);
        break;
      }
    }
    client->writing = false;
  }

  [[nodiscard]] async::Awaitable<void> reminder_loop() {
    while (!stopped_) {
      reminder_timer_.expires_after(std::chrono::seconds{5});
      auto [ec] = co_await reminder_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
      if (ec || stopped_) {
        break;
      }
      if (options_.reminders != nullptr) {
        auto due = co_await asio::co_spawn(options_.blocking_executor,
                                           options_.reminders->claim_due(now_ms()),
                                           asio::use_awaitable);
        if (due && !due->empty()) {
          for (const auto& item : *due) {
            broadcast_sse("reminder_due",
                          json{
                              {"id", item.id},
                              {"title", item.title},
                              {"dueAt", item.due_at_ms},
                          });
          }
        }
      }
      broadcast_sse("heartbeat", json{{"ts", now_ms()}});
    }
  }

  [[nodiscard]] async::Awaitable<std::optional<HttpRequest>> read_http_request(asio::ip::tcp::socket& socket) {
    asio::streambuf buf;
    auto [ec, bytes] = co_await asio::async_read_until(socket, buf, "\r\n\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec || bytes == 0) {
      co_return std::nullopt;
    }

    std::string raw(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + static_cast<std::ptrdiff_t>(bytes));
    buf.consume(bytes);

    const auto first_eol = raw.find("\r\n");
    if (first_eol == std::string::npos) {
      co_return std::nullopt;
    }
    const auto request_line = std::string_view{raw}.substr(0, first_eol);
    const auto sp1 = request_line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? std::string_view::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
      co_return std::nullopt;
    }

    HttpRequest req;
    req.method = std::string{request_line.substr(0, sp1)};
    const auto target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (const auto q = target.find('?'); q != std::string_view::npos) {
      req.path = std::string{target.substr(0, q)};
      req.query = std::string{target.substr(q + 1)};
    } else {
      req.path = std::string{target};
    }

    std::size_t content_length = 0;
    std::size_t line_start = first_eol + 2;
    while (line_start < raw.size()) {
      const auto line_end = raw.find("\r\n", line_start);
      if (line_end == std::string::npos || line_end == line_start) {
        break;
      }
      const auto header = std::string_view{raw}.substr(line_start, line_end - line_start);
      if (const auto colon = header.find(':'); colon != std::string_view::npos) {
        auto key = std::string{header.substr(0, colon)};
        std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto val = header.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
          val.remove_prefix(1);
        }
        if (key == "content-length") {
          try {
            content_length = static_cast<std::size_t>(std::stoull(std::string{val}));
          } catch (...) {
            co_return std::nullopt;
          }
        } else if (key == "origin") {
          req.origin = std::string{val};
        }
      }
      line_start = line_end + 2;
    }

    if (content_length > kMaxHttpBodyBytes) {
      co_return std::nullopt;
    }

    if (content_length > 0) {
      req.body.assign(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
      buf.consume(buf.size());
      if (req.body.size() < content_length) {
        const std::size_t need = content_length - req.body.size();
        const std::size_t prev = req.body.size();
        req.body.resize(content_length);
        auto [read_ec, _] = co_await asio::async_read(
            socket, asio::buffer(req.body.data() + prev, need), asio::as_tuple(asio::use_awaitable));
        if (read_ec) {
          co_return std::nullopt;
        }
      } else if (req.body.size() > content_length) {
        req.body.resize(content_length);
      }
    }
    co_return req;
  }

  [[nodiscard]] async::Awaitable<json> list_memory_json() {
    json notes = json::array();
    auto* backend = options_.assembly->longterm_memory_backend();
    if (backend == nullptr) {
      co_return notes;
    }
    for (const auto& [scope_key, scope_label] :
         std::array<std::pair<std::string, std::string>, 2>{{{std::string{kPersonalScopeKey}, "personal"},
                                                             {std::string{kSharedScopeKey}, "shared"}}}) {
      constexpr std::size_t kPageSize = 20;
      for (std::size_t offset = 0; offset < 100; offset += kPageSize) {
        auto entries = co_await asio::co_spawn(
            options_.blocking_executor,
            backend->list(memory::longterm::IndexRequest{.scope_key = scope_key, .limit = kPageSize, .offset = offset}),
            asio::use_awaitable);
        if (!entries || entries->empty()) {
          break;
        }
        const bool has_more = entries->size() > kPageSize;
        const std::size_t page_count = std::min(entries->size(), kPageSize);
        for (std::size_t i = 0; i < page_count; ++i) {
          auto full =
              co_await asio::co_spawn(options_.blocking_executor, backend->get((*entries)[i].key), asio::use_awaitable);
          if (!full || full->shadow) {
            continue;
          }
          notes.push_back(json{
              {"id", full->key.id},
              {"scope", scope_label},
              {"scopeKey", full->key.scope_key},
              {"kind", std::string{core::enum_name(full->kind)}},
              {"ownerId", scope_label == "shared" ? json(nullptr) : json("[\"desktop\",\"you\"]")},
              {"title", full->title},
              {"body", full->body},
              {"source", scope_label == "shared" ? "工作区共享" : "桌面 · 本地记忆"},
              {"updatedAt", to_ms(full->updated_at)},
          });
        }
        if (!has_more) {
          break;
        }
      }
    }
    co_return notes;
  }

  [[nodiscard]] async::Awaitable<json> list_reminders_json() {
    json out = json::array();
    if (options_.reminders == nullptr) {
      co_return out;
    }
    auto listed = co_await asio::co_spawn(options_.blocking_executor, options_.reminders->list(), asio::use_awaitable);
    if (!listed) {
      co_return out;
    }
    for (const auto& r : *listed) {
      out.push_back(json{
          {"id", r.id},
          {"title", r.title},
          {"dueAt", r.due_at_ms},
          {"completed", r.completed},
          {"notified", r.notified},
          {"createdAt", r.created_at_ms},
      });
    }
    co_return out;
  }

  [[nodiscard]] async::Awaitable<json> build_state_json() {
    json convs = json::array();
    json msgs = json::array();
    auto* store = options_.assembly->session_store();

    for (const auto& c : conversations_) {
      convs.push_back(json{
          {"id", c.id},
          {"sessionId", c.session_id},
          {"name", c.name},
          {"customName", c.custom_name},
          {"createdAt", c.created_at_ms},
      });
      if (store != nullptr) {
        auto history = co_await asio::co_spawn(
            options_.blocking_executor,
            store->load_tail(memory::session::SessionId{.value = c.session_id},
                             memory::session::AgentKey{.value = std::string{kAgentKey}},
                             128),
            asio::use_awaitable);
        if (history) {
          std::size_t idx = 0;
          for (const auto& m : *history) {
            if (m.role != core::Role::user && m.role != core::Role::assistant) {
              continue;
            }
            const auto text = extract_message_text(m);
            if (text.empty()) {
              continue;
            }
            msgs.push_back(json{
                {"id", std::format("{}:m:{}", c.id, idx++)},
                {"conversationId", c.id},
                {"sessionId", c.session_id},
                {"role", m.role == core::Role::user ? "user" : "assistant"},
                {"text", text},
                {"createdAt", c.created_at_ms + static_cast<std::int64_t>(idx) * 1000},
                {"state", "complete"},
            });
          }
        }
      }
    }

    json tasks_arr = json::array();
    for (const auto& [_, t] : tasks_) {
      json approval_obj = nullptr;
      if (t.approval_id) {
        if (auto it = approvals_.find(*t.approval_id); it != approvals_.end()) {
          approval_obj = json{
              {"id", it->second.id},
              {"status", it->second.status},
              {"target", it->second.target},
              {"tool", it->second.tool_name},
          };
        }
      }
      tasks_arr.push_back(json{
          {"id", t.id},
          {"conversationId", t.conversation_id},
          {"sessionId", t.session_id},
          {"title", t.title},
          {"status", t.status},
          {"activeCompanion", t.active_companion},
          {"activeTool", t.active_tool},
          {"createdAt", t.created_at_ms},
          {"approval", approval_obj},
      });
    }

    co_return json{
        {"mode", options_.demo_mode ? "demo" : "live"},
        {"workspace", options_.workspace_path},
        {"model", options_.route.primary.model},
        {"permission", permission_policy_},
        {"conversations", std::move(convs)},
        {"messages", std::move(msgs)},
        {"tasks", std::move(tasks_arr)},
        {"notes", co_await list_memory_json()},
        {"reminders", co_await list_reminders_json()},
    };
  }

  [[nodiscard]] async::Awaitable<void>
  run_agent_turn(std::string task_id, std::string conv_id, std::string session_id_text, std::string prompt) {
    struct StreamBridge final : provider::EventSink {
      Impl* self;
      std::string task_id;
      std::string conv_id;
      void on_text_delta(std::string_view delta) override {
        self->broadcast_sse("text_delta",
                            json{{"taskId", task_id}, {"conversationId", conv_id}, {"delta", std::string{delta}}});
      }
    };

    auto parsed_sid = core::parse_turn_id_hex(session_id_text);
    core::TurnId sid = parsed_sid ? *parsed_sid : core::TurnId{};

    StreamBridge sink{};
    sink.self = this;
    sink.task_id = task_id;
    sink.conv_id = conv_id;

    bootstrap::AgentSessionOptions session_opts{
        .executor = options_.strand,
        .blocking_executor = options_.blocking_executor,
        .assembly = options_.assembly,
        .config = options_.config,
        .provider = options_.provider,
        .route = options_.route,
        .mode = permission_policy_ == "allow"
                    ? permission::Mode::permissive
                    : (permission_policy_ == "deny" ? permission::Mode::sandboxed : permission::Mode::default_),
        .scope_key = std::string{kPersonalScopeKey},
        .agent_key = std::string{kAgentKey},
        .identity = "owner",
        .origin = "desktop",
        .stream = true,
        .session_id = sid,
        .event_sink = &sink,
    };

    auto session = bootstrap::AgentSession::create(std::move(session_opts));
    if (!session) {
      if (auto it = tasks_.find(task_id); it != tasks_.end()) {
        it->second.status = "failed";
      }
      broadcast_sse("task_updated",
                    json{{"taskId", task_id},
                         {"conversationId", conv_id},
                         {"status", "failed"},
                         {"error", std::string{session.error().message()}}});
      co_return;
    }

    auto cancel_signal = tasks_[task_id].cancel_signal;
    auto result = co_await asio::co_spawn(
        options_.strand,
        (*session)->run_prompt(agent::PromptRequest{.prompt = prompt}),
        asio::bind_cancellation_slot(cancel_signal->slot(), asio::use_awaitable));

    auto it = tasks_.find(task_id);
    if (!result) {
      const bool was_cancelled = result.error().kind() == core::ErrorKind::cancelled;
      const std::string final_status = was_cancelled ? "cancelled" : "failed";
      if (it != tasks_.end()) {
        it->second.status = final_status;
      }
      broadcast_sse("task_updated",
                    json{{"taskId", task_id},
                         {"conversationId", conv_id},
                         {"status", final_status},
                         {"error", std::string{result.error().message()}}});
      co_return;
    }

    if (it != tasks_.end()) {
      it->second.status = "completed";
      it->second.active_companion = "oran";
      it->second.active_tool.clear();
    }
    broadcast_sse("task_updated",
                  json{{"taskId", task_id},
                       {"conversationId", conv_id},
                       {"status", "completed"},
                       {"text", result->text}});
  }

  [[nodiscard]] async::Awaitable<void> handle_connection(asio::ip::tcp::socket socket) {
    auto req_opt = co_await read_http_request(socket);
    if (!req_opt) {
      co_return;
    }
    const auto& req = *req_opt;

    if (req.method == "OPTIONS") {
      auto resp = make_http_options(req.origin);
      co_await asio::async_write(socket, asio::buffer(resp), asio::as_tuple(asio::use_awaitable));
      co_return;
    }

    if (req.method == "GET" && req.path == "/api/events") {
      const auto headers = std::format(
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/event-stream\r\n"
          "Cache-Control: no-cache\r\n"
          "Connection: keep-alive\r\n"
          "Access-Control-Allow-Origin: {}\r\n\r\n"
          "event: connected\ndata: {{\"ok\":true}}\n\n",
          cors_origin_header(req.origin));
      auto [ec, _] = co_await asio::async_write(socket, asio::buffer(headers), asio::as_tuple(asio::use_awaitable));
      if (!ec) {
        sse_clients_.push_back(std::make_shared<SseClient>(std::move(socket)));
      }
      co_return;
    }

    std::string response;
    if (req.method == "GET" && req.path == "/api/health") {
      response = make_http_json(200,
                                json{
                                    {"ok", true},
                                    {"runtime", "orangutan-cpp26"},
                                    {"mode", options_.demo_mode ? "demo" : "live"},
                                    {"workspace", options_.workspace_path},
                                    {"model", options_.route.primary.model},
                                },
                                req.origin);
    } else if (req.method == "GET" && req.path == "/api/state") {
      response = make_http_json(200, co_await build_state_json(), req.origin);
    } else if (req.method == "POST" && req.path == "/api/conversations") {
      const auto id = make_id();
      auto body = json::parse(req.body, nullptr, false);
      const std::string name =
          body.is_object() && body.contains("name") && body["name"].is_string() && !body["name"].get<std::string>().empty()
              ? body["name"].get<std::string>()
              : "新的对话";
      ConversationRecord rec{
          .id = id,
          .session_id = id,
          .name = name,
          .custom_name = false,
          .created_at_ms = now_ms(),
      };
      conversations_.insert(conversations_.begin(), rec);
      save_conversations();
      response = make_http_json(201,
                                json{{"id", rec.id},
                                     {"sessionId", rec.session_id},
                                     {"name", rec.name},
                                     {"customName", rec.custom_name},
                                     {"createdAt", rec.created_at_ms}},
                                req.origin);
    } else if (req.method == "PATCH" && req.path.starts_with("/api/conversations/")) {
      const auto cid = url_decode(std::string_view{req.path}.substr(std::string_view{"/api/conversations/"}.size()));
      auto body = json::parse(req.body, nullptr, false);
      auto it = std::ranges::find_if(conversations_, [&](const auto& c) { return c.id == cid; });
      if (it == conversations_.end()) {
        response = make_http_json(404, json{{"error", "conversation not found"}}, req.origin);
      } else if (!body.is_object() || !body.contains("name") || !body["name"].is_string() ||
                 body["name"].get<std::string>().empty()) {
        response = make_http_json(400, json{{"error", "invalid conversation name"}}, req.origin);
      } else {
        it->name = body["name"].get<std::string>();
        it->custom_name = true;
        save_conversations();
        response = make_http_json(200, json{{"id", it->id}, {"name", it->name}}, req.origin);
      }
    } else if (req.method == "POST" && req.path.starts_with("/api/conversations/") &&
               req.path.ends_with("/messages")) {
      constexpr std::string_view prefix = "/api/conversations/";
      constexpr std::string_view suffix = "/messages";
      const auto cid =
          url_decode(std::string_view{req.path}.substr(prefix.size(), req.path.size() - prefix.size() - suffix.size()));
      auto body = json::parse(req.body, nullptr, false);
      auto it = std::ranges::find_if(conversations_, [&](const auto& c) { return c.id == cid; });
      if (it == conversations_.end()) {
        response = make_http_json(404, json{{"error", "conversation not found"}}, req.origin);
      } else if (!body.is_object() || !body.contains("text") || !body["text"].is_string() ||
                 body["text"].get<std::string>().empty()) {
        response = make_http_json(400, json{{"error", "message text is required"}}, req.origin);
      } else {
        const auto text = body["text"].get<std::string>();
        const bool busy = std::ranges::any_of(tasks_, [&](const auto& kv) {
          return kv.second.conversation_id == cid &&
                 (kv.second.status == "running" || kv.second.status == "awaiting_approval" ||
                  kv.second.status == "cancelling");
        });
        if (busy) {
          response = make_http_json(409, json{{"error", "conversation already has an active task"}}, req.origin);
        } else {
          if (!it->custom_name && it->name == "新的对话") {
            it->name = text.substr(0, std::min<std::size_t>(text.size(), 48));
            save_conversations();
          }
          const auto task_id = make_id();
          tasks_[task_id] = TaskRecord{
              .id = task_id,
              .conversation_id = cid,
              .session_id = it->session_id,
              .title = text.substr(0, std::min<std::size_t>(text.size(), 60)),
              .status = "running",
              .active_companion = "oran",
              .active_tool = {},
              .created_at_ms = now_ms(),
              .approval_id = std::nullopt,
              .cancel_signal = std::make_shared<asio::cancellation_signal>(),
          };
          asio::co_spawn(options_.strand, run_agent_turn(task_id, cid, it->session_id, text), asio::detached);
          response = make_http_json(201,
                                    json{{"taskId", task_id},
                                         {"conversationId", cid},
                                         {"sessionId", it->session_id},
                                         {"status", "running"}},
                                    req.origin);
        }
      }
    } else if (req.method == "POST" && req.path.starts_with("/api/tasks/") && req.path.ends_with("/stop")) {
      constexpr std::string_view prefix = "/api/tasks/";
      constexpr std::string_view suffix = "/stop";
      const auto tid =
          url_decode(std::string_view{req.path}.substr(prefix.size(), req.path.size() - prefix.size() - suffix.size()));
      auto it = tasks_.find(tid);
      if (it == tasks_.end()) {
        response = make_http_json(404, json{{"error", "task not found"}}, req.origin);
      } else {
        it->second.status = "cancelling";
        if (it->second.approval_id) {
          if (auto ait = approvals_.find(*it->second.approval_id); ait != approvals_.end() && ait->second.gate_timer) {
            ait->second.status = "cancelled";
            ait->second.decision = false;
            ait->second.gate_timer->cancel();
          }
        }
        if (it->second.cancel_signal) {
          it->second.cancel_signal->emit(asio::cancellation_type::terminal);
        }
        response = make_http_json(200, json{{"taskId", tid}, {"status", "cancelling"}}, req.origin);
      }
    } else if (req.method == "POST" && req.path.starts_with("/api/approvals/")) {
      const auto aid = url_decode(std::string_view{req.path}.substr(std::string_view{"/api/approvals/"}.size()));
      auto body = json::parse(req.body, nullptr, false);
      auto it = approvals_.find(aid);
      if (it == approvals_.end()) {
        response = make_http_json(404, json{{"error", "approval not found"}}, req.origin);
      } else if (it->second.status != "pending") {
        response = make_http_json(409, json{{"error", "approval already resolved"}}, req.origin);
      } else {
        const auto decision_str = body.is_object() ? body.value("decision", std::string{}) : std::string{};
        const bool approve = decision_str == "approve";
        if (!approve && decision_str != "reject") {
          response = make_http_json(400, json{{"error", "decision must be 'approve' or 'reject'"}}, req.origin);
        } else {
          it->second.decision = approve;
          it->second.status = approve ? "approved" : "rejected";
          if (auto tit = tasks_.find(it->second.task_id); tit != tasks_.end() && tit->second.status == "awaiting_approval") {
            tit->second.status = "running";
          }
          if (it->second.gate_timer) {
            it->second.gate_timer->cancel();
          }
          broadcast_sse("approval_resolved",
                        json{{"id", aid}, {"taskId", it->second.task_id}, {"status", it->second.status}});
          response = make_http_json(200, json{{"id", aid}, {"status", it->second.status}}, req.origin);
        }
      }
    } else if (req.method == "GET" && req.path == "/api/memory") {
      response = make_http_json(200, json{{"notes", co_await list_memory_json()}}, req.origin);
    } else if ((req.method == "POST" && req.path == "/api/memory") ||
               (req.method == "PUT" && req.path.starts_with("/api/memory/"))) {
      auto* backend = options_.assembly->longterm_memory_backend();
      auto body = json::parse(req.body, nullptr, false);
      if (backend == nullptr) {
        response = make_http_json(400, json{{"error", "memory backend disabled"}}, req.origin);
      } else if (!body.is_object() || !body.contains("title") || !body.contains("body")) {
        response = make_http_json(400, json{{"error", "title and body are required"}}, req.origin);
      } else {
        const std::string id =
            req.method == "PUT"
                ? url_decode(std::string_view{req.path}.substr(std::string_view{"/api/memory/"}.size()))
                : (body.contains("id") && body["id"].is_string() ? body["id"].get<std::string>() : make_id());
        const std::string scope = body.value("scope", std::string{"personal"});
        const std::string scope_key = scope == "shared" ? std::string{kSharedScopeKey} : std::string{kPersonalScopeKey};
        const auto now = core::time::now_utc();
        memory::longterm::Record record{
            .key = memory::longterm::RecordKey{.id = id, .scope_key = scope_key},
            .kind = scope == "shared" ? memory::longterm::RecordKind::project : memory::longterm::RecordKind::user,
            .title = body["title"].get<std::string>(),
            .body = body["body"].get<std::string>(),
            .created_at = now,
            .updated_at = now,
            .last_read_at = now,
            .importance = 0.6,
            .tags = {},
            .linked_record_ids = {},
            .shadow = false,
        };
        auto saved = co_await asio::co_spawn(options_.blocking_executor, backend->upsert(std::move(record)), asio::use_awaitable);
        if (!saved) {
          response = make_http_json(400, json{{"error", std::string{saved.error().message()}}}, req.origin);
        } else {
          broadcast_sse("memory_changed", json{{"companion", "moss"}});
          response = make_http_json(200,
                                    json{{"id", saved->key.id},
                                         {"scope", scope},
                                         {"title", saved->title},
                                         {"body", saved->body},
                                         {"updatedAt", to_ms(saved->updated_at)}},
                                    req.origin);
        }
      }
    } else if (req.method == "DELETE" && req.path.starts_with("/api/memory/")) {
      auto* backend = options_.assembly->longterm_memory_backend();
      const auto id = url_decode(std::string_view{req.path}.substr(std::string_view{"/api/memory/"}.size()));
      if (backend == nullptr) {
        response = make_http_json(400, json{{"error", "memory backend disabled"}}, req.origin);
      } else {
        const std::string scope_key =
            req.query.contains("scope=shared") ? std::string{kSharedScopeKey} : std::string{kPersonalScopeKey};
        auto removed = co_await asio::co_spawn(
            options_.blocking_executor,
            backend->remove(memory::longterm::RecordKey{.id = id, .scope_key = scope_key}),
            asio::use_awaitable);
        if (!removed) {
          response = make_http_json(400, json{{"error", std::string{removed.error().message()}}}, req.origin);
        } else {
          broadcast_sse("memory_changed", json{{"companion", "moss"}});
          response = make_http_json(200, json{{"deleted", id}}, req.origin);
        }
      }
    } else if (req.method == "GET" && req.path == "/api/reminders") {
      response = make_http_json(200, json{{"reminders", co_await list_reminders_json()}}, req.origin);
    } else if (req.method == "POST" && req.path == "/api/reminders") {
      auto body = json::parse(req.body, nullptr, false);
      if (options_.reminders == nullptr) {
        response = make_http_json(400, json{{"error", "reminders unavailable"}}, req.origin);
      } else if (!body.is_object() || !body.contains("title") || !body["title"].is_string() ||
                 body["title"].get<std::string>().empty()) {
        response = make_http_json(400, json{{"error", "reminder title is required"}}, req.origin);
      } else {
        const auto now = now_ms();
        const auto due = body.contains("dueAt") && body["dueAt"].is_number_integer()
                             ? body["dueAt"].get<std::int64_t>()
                             : (now + 30 * 60 * 1000);
        Reminder item{
            .id = make_id(),
            .title = body["title"].get<std::string>(),
            .due_at_ms = due,
            .completed = false,
            .notified = false,
            .created_at_ms = now,
        };
        auto added = co_await asio::co_spawn(options_.blocking_executor, options_.reminders->add(item), asio::use_awaitable);
        if (!added) {
          response = make_http_json(400, json{{"error", std::string{added.error().message()}}}, req.origin);
        } else {
          response = make_http_json(201,
                                    json{{"id", added->id},
                                         {"title", added->title},
                                         {"dueAt", added->due_at_ms},
                                         {"completed", added->completed},
                                         {"notified", added->notified},
                                         {"createdAt", added->created_at_ms}},
                                    req.origin);
        }
      }
    } else if (req.method == "POST" && req.path.starts_with("/api/reminders/") && req.path.ends_with("/done")) {
      constexpr std::string_view prefix = "/api/reminders/";
      constexpr std::string_view suffix = "/done";
      const auto id =
          url_decode(std::string_view{req.path}.substr(prefix.size(), req.path.size() - prefix.size() - suffix.size()));
      auto body = json::parse(req.body, nullptr, false);
      const bool done = body.is_object() ? body.value("completed", true) : true;
      auto updated = co_await asio::co_spawn(
          options_.blocking_executor, options_.reminders->mark_completed(id, done), asio::use_awaitable);
      response = updated ? make_http_json(200, json{{"id", id}, {"completed", done}}, req.origin)
                         : make_http_json(400, json{{"error", std::string{updated.error().message()}}}, req.origin);
    } else if (req.method == "DELETE" && req.path.starts_with("/api/reminders/")) {
      const auto id = url_decode(std::string_view{req.path}.substr(std::string_view{"/api/reminders/"}.size()));
      auto removed =
          co_await asio::co_spawn(options_.blocking_executor, options_.reminders->remove(id), asio::use_awaitable);
      response = removed ? make_http_json(200, json{{"deleted", id}}, req.origin)
                         : make_http_json(400, json{{"error", std::string{removed.error().message()}}}, req.origin);
    } else if (req.method == "PATCH" && req.path == "/api/settings") {
      auto body = json::parse(req.body, nullptr, false);
      if (body.is_object() && body.contains("permission") && body["permission"].is_string()) {
        const auto p = body["permission"].get<std::string>();
        if (p == "ask" || p == "allow" || p == "deny") {
          permission_policy_ = p;
        }
      }
      response = make_http_json(200, json{{"permission", permission_policy_}}, req.origin);
    } else {
      response = make_http_json(404, json{{"error", "not found"}}, req.origin);
    }

    co_await asio::async_write(socket, asio::buffer(response), asio::as_tuple(asio::use_awaitable));
  }

  PersonalServerOptions options_;
  asio::ip::tcp::acceptor acceptor_;
  asio::steady_timer reminder_timer_;
  bool stopped_{false};
  std::string permission_policy_{"ask"};
  std::vector<ConversationRecord> conversations_;
  std::unordered_map<std::string, TaskRecord> tasks_;
  std::unordered_map<std::string, ApprovalRecord> approvals_;
  std::vector<std::shared_ptr<SseClient>> sse_clients_;
};

core::Result<std::unique_ptr<PersonalServer>> PersonalServer::create(PersonalServerOptions options) {
  if (options.assembly == nullptr || options.config == nullptr || options.provider == nullptr) {
    return std::unexpected(core::Error::invalid_argument("personal server requires assembly, config, and provider"));
  }
  if (options.bind_host != "127.0.0.1" && options.bind_host != "::1") {
    return std::unexpected(core::Error::permission_denied("personal server binds to loopback only"));
  }

  std::error_code ec;
  const auto addr = asio::ip::make_address(options.bind_host, ec);
  if (ec) {
    return std::unexpected(core::Error::invalid_argument("invalid loopback bind address").with("detail", ec.message()));
  }

  asio::ip::tcp::acceptor acceptor{options.strand};
  const asio::ip::tcp::endpoint endpoint{addr, options.port};
  acceptor.open(endpoint.protocol(), ec);
  if (ec) {
    return std::unexpected(core::Error::io("failed to open TCP acceptor").with("detail", ec.message()));
  }
  acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
  acceptor.bind(endpoint, ec);
  if (ec) {
    return std::unexpected(core::Error::io("failed to bind loopback port").with("detail", ec.message()));
  }
  acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return std::unexpected(core::Error::io("failed to listen on loopback port").with("detail", ec.message()));
  }

  auto impl = std::make_unique<Impl>(std::move(options), std::move(acceptor));
  return std::unique_ptr<PersonalServer>(new PersonalServer(std::move(impl)));
}

PersonalServer::PersonalServer(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
PersonalServer::~PersonalServer() = default;

std::uint16_t PersonalServer::bound_port() const noexcept {
  return impl_->bound_port();
}

async::Awaitable<core::Result<void>> PersonalServer::run() {
  co_return co_await impl_->run();
}

void PersonalServer::stop() noexcept {
  impl_->stop();
}

}  // namespace orangutan::personal
