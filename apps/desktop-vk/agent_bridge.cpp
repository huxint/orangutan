#include "agent_bridge.hpp"
#include "primitives.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/storage/pool.hpp>

#include "../personal/local_provider.hpp"
#include "../personal/reminders.hpp"

namespace orangutan::vkui {
namespace {

using json = nlohmann::json;

constexpr std::string_view kPersonalScopeKey = "personal:owner";
constexpr std::string_view kSharedScopeKey = "workspace:shared";
constexpr std::string_view kAgentKey = "oran";

[[nodiscard]] std::int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string make_id() {
  if (auto tid = core::generate_turn_id()) {
    return core::format_turn_id_hex(*tid);
  }
  return std::to_string(now_ms());
}

[[nodiscard]] std::string summarize_target(std::string_view tool_name, std::string_view input_json) {
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

/// Splits a user prompt into clear team sub-tasks (splitting *tasks*, not tools).
[[nodiscard]] std::vector<SubTaskCard> plan_team_subtasks(std::string_view prompt, CompanionId lead) {
  std::vector<SubTaskCard> cards;
  const bool mentions_search = prompt.contains("搜索") || prompt.contains("调研") || prompt.contains("查") ||
                               prompt.contains("列出") || prompt.contains("对比") || prompt.contains("search");
  const bool mentions_write = prompt.contains("写") || prompt.contains("修改") || prompt.contains("生成") ||
                              prompt.contains("计划") || prompt.contains("方案") || prompt.contains("创建");
  const bool mentions_memory = prompt.contains("记住") || prompt.contains("偏好") || prompt.contains("总结") ||
                               prompt.contains("整理") || prompt.contains("记忆");
  const bool mentions_remind = prompt.contains("提醒") || prompt.contains("待办") || prompt.contains("日程") ||
                               prompt.contains("跟进");

  cards.push_back(SubTaskCard{
      .id = make_id(),
      .assignee = lead,
      .title = "拆解目标与汇总结论",
      .status = "running",
      .detail = "理解需求并协调小队同步推进",
  });

  if (mentions_search || (!mentions_write && !mentions_memory && !mentions_remind)) {
    cards.push_back(SubTaskCard{
        .id = make_id(),
        .assignee = CompanionId::scout,
        .title = "探索现状与关键线索",
        .status = "running",
        .detail = "梳理当前工作区上下文与相关信息",
    });
  }
  if (mentions_write) {
    cards.push_back(SubTaskCard{
        .id = make_id(),
        .assignee = CompanionId::fern,
        .title = "起草方案与落地内容",
        .status = "running",
        .detail = "编写结构化草稿或具体工作区改动",
    });
    cards.push_back(SubTaskCard{
        .id = make_id(),
        .assignee = CompanionId::pebble,
        .title = "复核细节与变更把关",
        .status = "waiting",
        .detail = "检查风险边界，涉及写入前由你确认",
    });
  }
  if (mentions_memory) {
    cards.push_back(SubTaskCard{
        .id = make_id(),
        .assignee = CompanionId::moss,
        .title = "沉淀个人偏好与笔记",
        .status = "running",
        .detail = "整理长期上下文存入本地 SQLite 记忆",
    });
  }
  if (mentions_remind) {
    cards.push_back(SubTaskCard{
        .id = make_id(),
        .assignee = CompanionId::pip,
        .title = "安排里程碑与提醒节点",
        .status = "running",
        .detail = "记录跟进时间点并在到期前轻声提醒",
    });
  }
  return cards;
}

}  // namespace

struct AgentBridge::Impl {
  mutable std::mutex mu;
  AgentSnapshot state;

  std::unique_ptr<async::Runtime> runtime;
  std::optional<asio::strand<asio::any_io_executor>> strand;
  config::Config cfg;
  std::optional<bootstrap::RuntimeAssembly> assembly;
  std::optional<storage::Pool> personal_pool;
  std::unique_ptr<personal::ReminderStore> reminders;
  std::optional<bootstrap::HttpProviderBackend> http_backend;
  personal::LocalDemoProvider demo_provider;
  provider::System* provider_ptr{nullptr};
  provider::Route route;

  std::unordered_map<std::string, std::vector<ChatMessage>> conv_messages;
  std::shared_ptr<asio::cancellation_signal> active_cancel;

  struct ApprovalGate {
    std::shared_ptr<asio::steady_timer> timer;
    std::optional<bool> decision;
  };
  std::unordered_map<std::string, ApprovalGate> approval_gates;
  bool started{false};

  void wire_hooks() {
    auto& bus = assembly->hook_bus();

    bus.subscribe(
        hook::Sink{
            .id = "vk-desktop-approval-gate",
            .observe = {},
            .decide = [this](hook::Event, hook::PayloadPtr payload)
                -> async::Awaitable<core::Result<hook::HookDecision>> {
              const auto* ask = std::get_if<hook::PermissionAskRenderedPayload>(payload.get());
              if (ask == nullptr) {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              std::string policy;
              {
                std::lock_guard lock{mu};
                policy = state.permission_policy;
              }
              if (policy == "allow") {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              if (policy == "deny") {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "denied_by_policy"};
              }

              const auto aid = make_id();
              auto timer = std::make_shared<asio::steady_timer>(*strand);
              timer->expires_at(std::chrono::steady_clock::time_point::max());
              {
                std::lock_guard lock{mu};
                approval_gates[aid] = ApprovalGate{.timer = timer, .decision = std::nullopt};
                state.active_mood = CompanionMood::guarding;
                state.status_line = "石墩正在等你确认关键操作";
                state.pending_approval = PendingApprovalView{
                    .id = aid,
                    .action_name = ask->tool_name,
                    .target = summarize_target(ask->tool_name, ask->input_json),
                    .reason = ask->decision_reason,
                };
              }

              auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
              static_cast<void>(ec);

              bool approved = false;
              {
                std::lock_guard lock{mu};
                if (auto it = approval_gates.find(aid); it != approval_gates.end()) {
                  approved = it->second.decision.value_or(false);
                  approval_gates.erase(it);
                }
                if (state.pending_approval && state.pending_approval->id == aid) {
                  state.pending_approval.reset();
                }
                state.active_mood = CompanionMood::working;
                state.status_line = approved ? "已确认，小队继续推进中…" : "已取消该操作";
              }
              if (approved) {
                co_return hook::HookDecision{.kind = hook::HookDecisionKind::proceed};
              }
              co_return hook::HookDecision{.kind = hook::HookDecisionKind::veto, .reason = "declined_in_desktop"};
            },
            .trusted_local = true,
        },
        {hook::Event::permission_ask_rendered});

    bus.subscribe(
        hook::Sink{
            .id = "vk-desktop-activity",
            .observe = [this](hook::Event ev, hook::PayloadPtr) -> async::Awaitable<void> {
              if (ev == hook::Event::memory_write_after || ev == hook::Event::memory_forget) {
                co_await refresh_memory_and_reminders();
              }
              co_return;
            },
            .decide = {},
            .trusted_local = false,
        },
        {hook::Event::memory_write_after, hook::Event::memory_forget});
  }

  async::Awaitable<void> refresh_memory_and_reminders() {
    std::vector<MemoryNoteView> notes;
    if (auto* backend = assembly->longterm_memory_backend(); backend != nullptr) {
      for (const auto& [scope_key, label] :
           std::array<std::pair<std::string, std::string>, 2>{{{std::string{kPersonalScopeKey}, "personal"},
                                                               {std::string{kSharedScopeKey}, "shared"}}}) {
        constexpr std::size_t kPage = 20;
        for (std::size_t offset = 0; offset < 60; offset += kPage) {
          auto entries = co_await asio::co_spawn(
              runtime->cpu_executor(),
              backend->list(memory::longterm::IndexRequest{.scope_key = scope_key, .limit = kPage, .offset = offset}),
              asio::use_awaitable);
          if (!entries || entries->empty()) {
            break;
          }
          const bool more = entries->size() > kPage;
          const std::size_t take = std::min(entries->size(), kPage);
          for (std::size_t i = 0; i < take; ++i) {
            auto full = co_await asio::co_spawn(runtime->cpu_executor(), backend->get((*entries)[i].key),
                                                asio::use_awaitable);
            if (!full || full->shadow) {
              continue;
            }
            notes.push_back(MemoryNoteView{
                .id = full->key.id,
                .scope = label,
                .title = full->title,
                .body = full->body,
            });
          }
          if (!more) {
            break;
          }
        }
      }
    }

    std::vector<ReminderView> rems;
    if (reminders != nullptr) {
      auto listed = co_await asio::co_spawn(runtime->cpu_executor(), reminders->list(), asio::use_awaitable);
      if (listed) {
        for (const auto& r : *listed) {
          rems.push_back(ReminderView{
              .id = r.id,
              .title = r.title,
              .due_at_ms = r.due_at_ms,
              .completed = r.completed,
          });
        }
      }
    }

    std::lock_guard lock{mu};
    state.notes = std::move(notes);
    state.reminders = std::move(rems);
  }
};

AgentBridge::AgentBridge() : impl_{std::make_unique<Impl>()} {}

AgentBridge::~AgentBridge() {
  stop();
}

bool AgentBridge::start(AgentBridgeOptions opts, std::string& error_out) {
  std::error_code fs_ec;
  const auto workspace_abs = std::filesystem::weakly_canonical(opts.workspace_path, fs_ec);
  if (fs_ec || !std::filesystem::is_directory(workspace_abs)) {
    error_out = std::format("invalid workspace directory '{}'", opts.workspace_path);
    return false;
  }
  const auto state_dir =
      opts.state_dir.empty() ? (workspace_abs / ".orangutan") : std::filesystem::path{opts.state_dir};
  std::filesystem::create_directories(state_dir, fs_ec);

  if (!opts.config_path.empty()) {
    auto loaded = config::Config::load_file(opts.config_path);
    if (!loaded) {
      error_out = std::string{loaded.error().message()};
      return false;
    }
    impl_->cfg = std::move(*loaded);
  } else {
    auto loaded = config::Config::parse(R"({"runtime":{"workers":4,"prompt":{"active_tools":"defaults"}}})");
    if (!loaded) {
      error_out = std::string{loaded.error().message()};
      return false;
    }
    impl_->cfg = std::move(*loaded);
    opts.demo_mode = true;
  }

  impl_->runtime = std::make_unique<async::Runtime>(async::RuntimeConfig{
      .io_workers = 1,
      .cpu_workers = static_cast<std::size_t>(std::max<std::int64_t>(2, impl_->cfg.runtime().workers)),
  });
  impl_->strand.emplace(impl_->runtime->make_strand());

  auto built_assembly = bootstrap::RuntimeAssembly::build(
      workspace_abs.string(), impl_->runtime->cpu_executor(),
      bootstrap::RuntimeAssemblyOptions{
          .audit_db_path = (state_dir / "audit.db").string(),
          .sessions_db_path = (state_dir / "sessions.db").string(),
          .longterm_memory_db_path = (state_dir / "memory.db").string(),
      });
  if (!built_assembly) {
    error_out = std::string{built_assembly.error().message()};
    return false;
  }
  impl_->assembly.emplace(std::move(*built_assembly));

  auto pool = storage::Pool::open(impl_->runtime->cpu_executor(),
                                  storage::PoolOptions{
                                      .path = (state_dir / "personal.db").string(),
                                      .reader_count = 1,
                                      .statement_cache_capacity = 8,
                                  });
  if (!pool) {
    error_out = std::string{pool.error().message()};
    return false;
  }
  impl_->personal_pool.emplace(std::move(*pool));
  impl_->reminders = std::make_unique<personal::ReminderStore>(*impl_->personal_pool);

  if (opts.demo_mode) {
    impl_->provider_ptr = &impl_->demo_provider;
    impl_->route.primary.profile = "local-demo";
    impl_->route.primary.model = "orangutan-local-demo";
  } else {
    auto http_built = bootstrap::HttpProviderBackend::build(
        impl_->cfg,
        bootstrap::HttpProviderBackendOptions{
            .blocking_executor = impl_->runtime->cpu_executor(),
            .request_timeout = std::chrono::milliseconds{impl_->cfg.runtime().request_timeout_ms},
            .max_stream_bytes = static_cast<std::uint64_t>(impl_->cfg.runtime().stream.max_bytes),
        });
    if (!http_built) {
      error_out = std::string{http_built.error().message()};
      return false;
    }
    impl_->http_backend.emplace(std::move(*http_built));
    impl_->provider_ptr = &impl_->http_backend->system();
    impl_->route = impl_->http_backend->route();
  }

  impl_->wire_hooks();

  {
    std::lock_guard lock{impl_->mu};
    const auto first_id = make_id();
    impl_->state.demo_mode = opts.demo_mode;
    impl_->state.model_name = impl_->route.primary.model;
    impl_->state.workspace_path = workspace_abs.string();
    impl_->state.state_dir = state_dir.string();
    impl_->state.active_conversation_id = first_id;
    impl_->state.conversations = {ConversationSummary{.id = first_id, .title = "和 Oran 聊聊"}};
    impl_->conv_messages[first_id] = {};
  }

  if (auto r = impl_->runtime->start(); !r) {
    error_out = std::string{r.error().message()};
    return false;
  }
  impl_->started = true;

  asio::co_spawn(
      *impl_->strand,
      [this]() -> async::Awaitable<void> {
        static_cast<void>(
            co_await asio::co_spawn(impl_->runtime->cpu_executor(), impl_->reminders->migrate(), asio::use_awaitable));
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);

  return true;
}

void AgentBridge::stop() {
  if (!impl_->started) {
    return;
  }
  stop_active_turn();
  if (impl_->runtime) {
    static_cast<void>(impl_->runtime->stop_and_join());
  }
  impl_->started = false;
}

AgentSnapshot AgentBridge::snapshot() const {
  std::lock_guard lock{impl_->mu};
  return impl_->state;
}

void AgentBridge::set_lead_companion(CompanionId id) {
  std::lock_guard lock{impl_->mu};
  impl_->state.lead_companion = id;
  const auto& p = companion_profile(id);
  impl_->state.status_line = std::format("{}（{}）现在陪在你身边", p.name, p.nickname);
}

void AgentBridge::set_permission_policy(std::string policy) {
  if (policy != "ask" && policy != "allow" && policy != "deny") {
    return;
  }
  std::lock_guard lock{impl_->mu};
  impl_->state.permission_policy = std::move(policy);
}

void AgentBridge::new_conversation() {
  std::lock_guard lock{impl_->mu};
  if (impl_->state.busy) {
    return;
  }
  const auto id = make_id();
  impl_->state.conversations.insert(impl_->state.conversations.begin(),
                                    ConversationSummary{.id = id, .title = "新的协作对话"});
  impl_->state.active_conversation_id = id;
  impl_->conv_messages[id] = {};
  impl_->state.messages.clear();
  impl_->state.subtasks.clear();
  impl_->state.active_mood = CompanionMood::idle;
  impl_->state.status_line = "新对话已开启，说说你的目标吧";
}

void AgentBridge::select_conversation(std::string id) {
  std::lock_guard lock{impl_->mu};
  if (impl_->state.busy) {
    return;
  }
  impl_->state.active_conversation_id = id;
  if (const auto it = impl_->conv_messages.find(id); it != impl_->conv_messages.end()) {
    impl_->state.messages = it->second;
  } else {
    impl_->state.messages.clear();
  }
  impl_->state.subtasks.clear();
}

void AgentBridge::send_message(std::string text) {
  if (text.empty() || !impl_->started) {
    return;
  }
  std::string conv_id;
  std::string assistant_msg_id = make_id();
  CompanionId lead = CompanionId::oran;
  {
    std::lock_guard lock{impl_->mu};
    if (impl_->state.busy) {
      return;
    }
    impl_->state.busy = true;
    impl_->state.active_mood = CompanionMood::thinking;
    lead = impl_->state.lead_companion;
    conv_id = impl_->state.active_conversation_id;

    // Update default conversation title with first user prompt.
    for (auto& c : impl_->state.conversations) {
      if (c.id == conv_id && (c.title == "和 Oran 聊聊" || c.title == "新的协作对话")) {
        c.title = text.substr(0, std::min<std::size_t>(text.size(), 36));
        break;
      }
    }

    auto& list = impl_->conv_messages[conv_id];
    list.push_back(ChatMessage{.id = make_id(), .is_user = true, .speaker = lead, .text = text, .streaming = false});
    list.push_back(
        ChatMessage{.id = assistant_msg_id, .is_user = false, .speaker = lead, .text = {}, .streaming = true});
    impl_->state.messages = list;
    impl_->state.subtasks = plan_team_subtasks(text, lead);
    impl_->state.status_line =
        std::format("{} 正在拆解任务并调度小队协作…", companion_profile(lead).nickname);
  }

  auto cancel = std::make_shared<asio::cancellation_signal>();
  {
    std::lock_guard lock{impl_->mu};
    impl_->active_cancel = cancel;
  }

  asio::co_spawn(
      *impl_->strand,
      [this, conv_id, assistant_msg_id, text, cancel]() -> async::Awaitable<void> {
        struct UiSink final : provider::EventSink {
          Impl* self;
          std::string conv_id;
          std::string msg_id;

          void on_text_delta(std::string_view delta) override {
            std::lock_guard lock{self->mu};
            self->state.active_mood = CompanionMood::working;
            auto& msgs = self->conv_messages[conv_id];
            for (auto& m : msgs) {
              if (m.id == msg_id) {
                m.text.append(delta);
                break;
              }
            }
            if (self->state.active_conversation_id == conv_id) {
              self->state.messages = msgs;
            }
          }
        };

        UiSink sink{};
        sink.self = impl_.get();
        sink.conv_id = conv_id;
        sink.msg_id = assistant_msg_id;

        auto parsed_sid = core::parse_turn_id_hex(conv_id);
        core::TurnId sid = parsed_sid ? *parsed_sid : core::TurnId{};

        std::string policy;
        {
          std::lock_guard lock{impl_->mu};
          policy = impl_->state.permission_policy;
        }

        auto session = bootstrap::AgentSession::create(bootstrap::AgentSessionOptions{
            .executor = *impl_->strand,
            .blocking_executor = impl_->runtime->cpu_executor(),
            .assembly = &*impl_->assembly,
            .config = &impl_->cfg,
            .provider = impl_->provider_ptr,
            .route = impl_->route,
            .mode = policy == "allow" ? permission::Mode::permissive
                                      : (policy == "deny" ? permission::Mode::sandboxed : permission::Mode::default_),
            .scope_key = std::string{kPersonalScopeKey},
            .agent_key = std::string{kAgentKey},
            .identity = "owner",
            .origin = "desktop-vk",
            .stream = true,
            .session_id = sid,
            .event_sink = &sink,
        });

        std::string final_text;
        bool ok = false;
        if (!session) {
          final_text = std::format("无法创建会话：{}", session.error().message());
        } else {
          auto res = co_await asio::co_spawn(
              *impl_->strand,
              (*session)->run_prompt(agent::PromptRequest{.prompt = text}),
              asio::bind_cancellation_slot(cancel->slot(), asio::use_awaitable));
          if (res) {
            ok = true;
            final_text = std::move(res->text);
          } else if (res.error().kind() == core::ErrorKind::cancelled) {
            final_text = "（已停止当前协作任务）";
          } else {
            final_text = std::format("执行中断：{}", res.error().message());
          }
        }

        {
          std::lock_guard lock{impl_->mu};
          auto& msgs = impl_->conv_messages[conv_id];
          for (auto& m : msgs) {
            if (m.id == assistant_msg_id) {
              if (m.text.empty() || !ok) {
                m.text = final_text;
              }
              m.streaming = false;
              break;
            }
          }
          if (impl_->state.active_conversation_id == conv_id) {
            impl_->state.messages = msgs;
          }
          for (auto& st : impl_->state.subtasks) {
            st.status = ok ? "done" : "waiting";
          }
          impl_->state.busy = false;
          impl_->state.active_mood = ok ? CompanionMood::celebrating : CompanionMood::idle;
          impl_->state.status_line = ok ? "小队已完成本轮子任务协作" : "已暂停，随时可以继续";
          impl_->active_cancel.reset();
        }
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::stop_active_turn() {
  if (!impl_->started) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this]() -> async::Awaitable<void> {
        std::shared_ptr<asio::cancellation_signal> cancel;
        std::vector<std::shared_ptr<asio::steady_timer>> timers;
        {
          std::lock_guard lock{impl_->mu};
          cancel = impl_->active_cancel;
          for (auto& [_, g] : impl_->approval_gates) {
            g.decision = false;
            if (g.timer) {
              timers.push_back(g.timer);
            }
          }
        }
        for (auto& t : timers) {
          t->cancel();
        }
        if (cancel) {
          cancel->emit(asio::cancellation_type::terminal);
        }
        co_return;
      },
      asio::detached);
}

void AgentBridge::resolve_approval(std::string approval_id, bool approve) {
  if (!impl_->started) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, approval_id = std::move(approval_id), approve]() -> async::Awaitable<void> {
        std::shared_ptr<asio::steady_timer> timer;
        {
          std::lock_guard lock{impl_->mu};
          if (auto it = impl_->approval_gates.find(approval_id); it != impl_->approval_gates.end()) {
            it->second.decision = approve;
            timer = it->second.timer;
          }
        }
        if (timer) {
          timer->cancel();
        }
        co_return;
      },
      asio::detached);
}

void AgentBridge::add_memory_note(std::string title, std::string body, std::string scope) {
  if (!impl_->started || title.empty() || body.empty()) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, title = std::move(title), body = std::move(body), scope = std::move(scope)]() -> async::Awaitable<void> {
        if (auto* backend = impl_->assembly->longterm_memory_backend(); backend != nullptr) {
          const auto now = core::time::now_utc();
          const std::string sk = scope == "shared" ? std::string{kSharedScopeKey} : std::string{kPersonalScopeKey};
          memory::longterm::Record rec{
              .key = memory::longterm::RecordKey{.id = make_id(), .scope_key = sk},
              .kind = scope == "shared" ? memory::longterm::RecordKind::project : memory::longterm::RecordKind::user,
              .title = title,
              .body = body,
              .created_at = now,
              .updated_at = now,
              .last_read_at = now,
              .importance = 0.7,
              .tags = {},
              .linked_record_ids = {},
              .shadow = false,
          };
          static_cast<void>(
              co_await asio::co_spawn(impl_->runtime->cpu_executor(), backend->upsert(std::move(rec)), asio::use_awaitable));
        }
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::delete_memory_note(std::string id, std::string scope) {
  if (!impl_->started) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, id = std::move(id), scope = std::move(scope)]() -> async::Awaitable<void> {
        if (auto* backend = impl_->assembly->longterm_memory_backend(); backend != nullptr) {
          const std::string sk = scope == "shared" ? std::string{kSharedScopeKey} : std::string{kPersonalScopeKey};
          static_cast<void>(co_await asio::co_spawn(
              impl_->runtime->cpu_executor(),
              backend->remove(memory::longterm::RecordKey{.id = id, .scope_key = sk}), asio::use_awaitable));
        }
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::add_reminder(std::string title, std::int64_t due_at_ms) {
  if (!impl_->started || title.empty()) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, title = std::move(title), due_at_ms]() -> async::Awaitable<void> {
        personal::Reminder rem{
            .id = make_id(),
            .title = title,
            .due_at_ms = due_at_ms,
            .completed = false,
            .notified = false,
            .created_at_ms = now_ms(),
        };
        static_cast<void>(
            co_await asio::co_spawn(impl_->runtime->cpu_executor(), impl_->reminders->add(std::move(rem)),
                                    asio::use_awaitable));
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::toggle_reminder(std::string id, bool completed) {
  if (!impl_->started) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, id = std::move(id), completed]() -> async::Awaitable<void> {
        static_cast<void>(co_await asio::co_spawn(
            impl_->runtime->cpu_executor(), impl_->reminders->mark_completed(id, completed), asio::use_awaitable));
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::delete_reminder(std::string id) {
  if (!impl_->started) {
    return;
  }
  asio::co_spawn(
      *impl_->strand,
      [this, id = std::move(id)]() -> async::Awaitable<void> {
        static_cast<void>(
            co_await asio::co_spawn(impl_->runtime->cpu_executor(), impl_->reminders->remove(id), asio::use_awaitable));
        co_await impl_->refresh_memory_and_reminders();
      },
      asio::detached);
}

void AgentBridge::seed_showcase_state() {
  std::lock_guard lock{impl_->mu};
  const std::string cid = impl_->state.active_conversation_id;
  impl_->state.conversations = {
      ConversationSummary{.id = cid, .title = "梳理本周项目发布计划"},
      ConversationSummary{.id = "conv-2", .title = "整理个人工作习惯与记忆"},
      ConversationSummary{.id = "conv-3", .title = "纯 Vulkan 桌面端视觉打磨"},
  };
  impl_->state.messages = {
      ChatMessage{
          .id = "m1",
          .is_user = true,
          .speaker = CompanionId::oran,
          .text = "帮我把「v2.0 发布准备」拆给小队并行推进：调研遗留事项、起草发布清单、顺便记下我的偏好，并加一个今晚 20:00 的复查提醒。",
          .streaming = false,
      },
      ChatMessage{
          .id = "m2",
          .is_user = false,
          .speaker = CompanionId::oran,
          .text = "好呀！我已经把这次发布目标拆成了 4 条并行子任务交给大家，每个伙伴各自负责一块任务而不是切碎工具：\n\n"
                  "1. 阿探 (Scout) 已摸清当前仓库与构建状态，确认 15 个核心测试套件全部通过。\n"
                  "2. 小蕨 (Fern) 正在起草精简版的发布检查清单与更新摘要。\n"
                  "3. 小苔 (Moss) 已把「中文先给结论」的偏好沉淀进本地 SQLite 记忆库。\n"
                  "4. 啾啾 (Pip) 已为你记下今晚 20:00 的发布复查提醒。\n\n"
                  "下面小蕨准备将清单写入工作区，石墩正在等你确认是否允许写入。",
          .streaming = false,
      },
  };
  impl_->conv_messages[cid] = impl_->state.messages;
  impl_->state.subtasks = {
      SubTaskCard{.id = "st1", .assignee = CompanionId::scout, .title = "核查仓库与测试状态", .status = "done", .detail = "15/15 核心测试通过"},
      SubTaskCard{.id = "st2", .assignee = CompanionId::fern, .title = "起草 v2.0 发布清单", .status = "running", .detail = "等待写入工作区确认"},
      SubTaskCard{.id = "st3", .assignee = CompanionId::moss, .title = "沉淀中文结论偏好", .status = "done", .detail = "已写入本地 SQLite"},
      SubTaskCard{.id = "st4", .assignee = CompanionId::pip, .title = "安排 20:00 复查提醒", .status = "done", .detail = "已加入提醒列表"},
  };
  impl_->state.pending_approval = PendingApprovalView{
      .id = "appr-demo",
      .action_name = "FileWrite",
      .target = "RELEASE_CHECKLIST.md",
      .reason = "小蕨请求将整理好的发布清单写入当前工作区",
  };
  impl_->state.notes = {
      MemoryNoteView{.id = "n1", .scope = "personal", .title = "回复风格偏好", .body = "优先用中文先给结论，细节展开放在后面，保持直接清爽。"},
      MemoryNoteView{.id = "n2", .scope = "shared", .title = "纯 Vulkan 桌面端视觉原则", .body = "暖纸张底色 + 陶土红点缀 + 清晰层次，拒绝冗余边框与花哨堆砌。"},
      MemoryNoteView{.id = "n3", .scope = "personal", .title = "小队协作约定", .body = "按子任务目标拆给不同伙伴推进，不把工具生硬割裂成孤岛。"},
  };
  impl_->state.reminders = {
      ReminderView{.id = "r1", .title = "今晚 20:00 复查 v2.0 发布检查清单", .due_at_ms = now_ms() + 3600000, .completed = false},
      ReminderView{.id = "r2", .title = "和石墩一起复核权限默认策略", .due_at_ms = now_ms() + 7200000, .completed = false},
      ReminderView{.id = "r3", .title = "确认 15 个 C++26 单元测试全部变绿", .due_at_ms = now_ms() - 1800000, .completed = true},
  };
  impl_->state.active_mood = CompanionMood::guarding;
  impl_->state.status_line = "小队并行推进中 · 等待确认写入";
}

}  // namespace orangutan::vkui
