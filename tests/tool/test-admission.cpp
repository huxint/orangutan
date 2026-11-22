#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async/channel.hpp>
#include <oran/hook.hpp>
#include <oran/io/file.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

struct Call {
  Call(std::string name, std::string input, tool::DispatchContext context)
      : name{std::move(name)}, input{std::move(input)}, context{std::move(context)} {}

  std::string name;
  std::string input;
  tool::DispatchContext context;
  asio::cancellation_signal cancellation;
  std::optional<core::Result<tool::Output>> result;
  std::exception_ptr failure;
};

class Admission {
public:
  Admission() {
    rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "*"});
    std::filesystem::create_directory(root);
    auto created = tool::Workspace::create(root.string());
    REQUIRE(created.has_value());
    workspace = std::move(*created);
    REQUIRE(tool::register_file_read(registry).has_value());
    REQUIRE(tool::register_file_write(registry).has_value());
    REQUIRE(tool::register_file_edit(registry).has_value());
    bus.subscribe(hook::Sink{
                      .id = "admission",
                      .observe = [this](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<void> {
                        const auto& after = std::get<hook::ToolAfterPayload>(*payload);
                        completed.push_back(after);
                        if (!after.succeeded) {
                          co_return;
                        }
                        executed.push_back(after);
                        // Completion observations finish before the path lock is released.
                        if (after.who.identity == "holder") {
                          static_cast<void>(co_await release.receive());
                        }
                      },
                      .decide = [this](hook::Event event,
                                       hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
                        if (event == hook::Event::permission_ask_rendered) {
                          ++approval_requests;
                          co_return hook::HookDecision{.reason = "test operator"};
                        }
                        const auto& before = std::get<hook::ToolBeforePayload>(*payload);
                        if (auto it = decisions.find(before.who.identity); it != decisions.end()) {
                          co_return it->second;
                        }
                        co_return hook::HookDecision{};
                      },
                      .trusted_local = true,
                  },
                  {hook::Event::tool_before, hook::Event::tool_after, hook::Event::permission_ask_rendered});
  }

  ~Admission() {
    release.close();
    poll();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::shared_ptr<Call>
  start(std::string name, std::string input, std::string identity, permission::AuditSink* audit_sink = nullptr) {
    auto context = tool::DispatchContext::for_now(io.get_executor(),
                                                  rules,
                                                  audit_sink ? *audit_sink : audit,
                                                  "scope",
                                                  "agent",
                                                  std::move(identity));
    context.workspace = &*workspace;
    context.path_locks = &locks;
    context.bus = &bus;
    auto call = std::make_shared<Call>(std::move(name), std::move(input), std::move(context));
    asio::co_spawn(io,
                   registry.dispatch(call->name, call->input, call->context),
                   asio::bind_cancellation_slot(call->cancellation.slot(),
                                                [call](std::exception_ptr failure, core::Result<tool::Output> result) {
                                                  call->failure = failure;
                                                  call->result = std::move(result);
                                                }));
    return call;
  }

  void rewrite(std::string identity, std::string input) {
    auto& decision = decisions[std::move(identity)];
    decision.kind = hook::HookDecisionKind::rewrite;
    decision.rewritten_input_json = std::move(input);
  }

  void poll() {
    io.restart();
    io.poll();
  }

  void finish_holder() {
    REQUIRE(release.try_send(true).has_value());
    poll();
  }

  std::string contents(std::string_view path) const {
    std::ifstream stream{root / path};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }

  std::filesystem::path root{
      std::filesystem::temp_directory_path() /
      ("oran-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  asio::io_context io;
  async::Channel<bool> release{io.get_executor(), 1};
  tool::Registry registry;
  tool::PathLocks locks;
  std::optional<tool::Workspace> workspace;
  permission::RuleSet rules;
  permission::RecordingAuditSink audit;
  hook::Bus bus{{.advisory_timeout = 2s}};
  std::map<std::string, hook::HookDecision> decisions;
  /// Successful completions: calls whose executor ran and returned.
  std::vector<hook::ToolAfterPayload> executed;
  std::vector<hook::ToolAfterPayload> completed;
  std::size_t approval_requests{0};
};

void require_success(const std::shared_ptr<Call>& call) {
  REQUIRE_FALSE(call->failure);
  REQUIRE(call->result.has_value());
  INFO((call->result->has_value() ? "success" : call->result->error().message()));
  REQUIRE(call->result->has_value());
}

class FailedAudit final : public permission::AuditSink {
public:
  async::Awaitable<core::Result<void>> record(permission::AuditEvent) override {
    co_return std::unexpected(core::Error::internal("audit unavailable"));
  }
};

async::Awaitable<core::Result<tool::Output>> read_prepared(std::unique_ptr<std::string> prefix,
                                                        tool::DispatchContext& ctx) {
  REQUIRE(ctx.resolved_path.has_value());
  REQUIRE(ctx.resolved_path->authority.has_value());
  auto file = ctx.resolved_path->authority->open_file(
      {.relative_path = ctx.resolved_path->authority_relative_path});
  REQUIRE(file.has_value());
  auto read = co_await orangutan::io::read_text_file_ranged(ctx.executor, std::move(*file));
  REQUIRE(read.has_value());
  co_return tool::Output{.text = *prefix + read->text};
}

}  // namespace

TEST_CASE("Tool admission excludes paths after input rewrite", "[integration][tool][admission][lock]") {
  Admission fixture;
  const std::string first_input = R"({"path":"shared.txt","content":"first"})";
  const std::string second_input = R"({"path":"./shared.txt","content":"second"})";
  fixture.rewrite("holder", first_input);
  fixture.rewrite("second", second_input);
  auto first = fixture.start("FileWrite", R"({"path":"a.txt","content":"unused"})", "holder");
  fixture.poll();
  auto second = fixture.start("FileWrite", R"({"path":"b.txt","content":"unused"})", "second");
  fixture.poll();
  const auto admitted_while_held = fixture.executed.size();
  const auto audited_while_held = fixture.audit.events().size();
  fixture.finish_holder();

  require_success(first);
  require_success(second);
  REQUIRE(admitted_while_held == 1);
  REQUIRE(audited_while_held == 1);
  REQUIRE(fixture.contents("shared.txt") == "second");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "a.txt"));
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "b.txt"));
  REQUIRE(fixture.executed[0].input_json == first_input);
  REQUIRE(fixture.executed[1].input_json == second_input);
  REQUIRE(fixture.audit.events()[1].input_hash == permission::ApprovalAuthority::input_hash(second_input));
}

TEST_CASE("Tool admission permits overlap after rewriting to distinct paths", "[integration][tool][admission][lock]") {
  Admission fixture;
  fixture.rewrite("holder", R"({"path":"a.txt","content":"first"})");
  fixture.rewrite("second", R"({"path":"b.txt","content":"second"})");
  const std::string original = R"({"path":"shared.txt","content":"unused"})";
  auto first = fixture.start("FileWrite", original, "holder");
  fixture.poll();
  auto second = fixture.start("FileWrite", original, "second");
  fixture.poll();
  const bool overlapped = second->result.has_value() && !first->result.has_value();
  fixture.finish_holder();

  require_success(first);
  require_success(second);
  REQUIRE(overlapped);
  REQUIRE(fixture.contents("a.txt") == "first");
  REQUIRE(fixture.contents("b.txt") == "second");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "shared.txt"));
}

TEST_CASE("Tool admission resolves a queued read after the writer creates its target",
          "[integration][tool][admission][workspace]") {
  Admission fixture;
  fixture.rewrite("reader", R"({"path":"created.txt"})");
  auto writer = fixture.start("FileWrite", R"({"path":"created.txt","content":"created under lock"})", "holder");
  fixture.poll();
  auto reader = fixture.start("FileRead", R"({"path":"original.txt"})", "reader");
  fixture.poll();
  const bool reader_waited = !reader->result.has_value();
  fixture.finish_holder();

  require_success(writer);
  require_success(reader);
  REQUIRE(reader_waited);
  REQUIRE(reader->result->value().text.contains("created under lock"));
  REQUIRE(fixture.completed.size() == 2);
}

TEST_CASE("Tool admission veto finishes without waiting for a held path", "[integration][tool][admission][hook]") {
  Admission fixture;
  fixture.decisions["veto"].kind = hook::HookDecisionKind::veto;
  fixture.decisions["veto"].reason = "blocked";
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto vetoed = fixture.start("FileWrite", R"({"path":"shared.txt","content":"veto"})", "veto");
  fixture.poll();
  const bool finished_before_release = vetoed->result.has_value();
  fixture.finish_holder();

  require_success(holder);
  REQUIRE(finished_before_release);
  REQUIRE_FALSE(vetoed->failure);
  REQUIRE_FALSE(vetoed->result->has_value());
  REQUIRE(vetoed->result->error().kind() == core::ErrorKind::permission_denied);
  REQUIRE(fixture.executed.size() == 1);
  REQUIRE(fixture.completed.size() == 2);
  REQUIRE(fixture.contents("shared.txt") == "holder");
}

TEST_CASE("Tool admission cancellation preserves approval and lets the next caller proceed",
          "[integration][tool][admission][cancellation][approval]") {
  Admission fixture;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string input = R"({"path":"shared.txt","content":"approved"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = input,
                                                         .identity = "approved",
                                                         .ttl = 60s,
                                                         .replay_max = 1},
                               core::time::now_utc());
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto cancelled = fixture.start("FileWrite", input, "approved");
  cancelled->context.rules = {};
  cancelled->context.approval_broker = &*broker;
  cancelled->context.approval_token = &token;
  fixture.poll();
  const bool waited = !cancelled->result.has_value();
  cancelled->cancellation.emit(asio::cancellation_type::all);
  fixture.poll();
  const bool cancelled_while_held = cancelled->result.has_value();
  auto successor = fixture.start("FileWrite", input, "approved");
  successor->context.rules = {};
  successor->context.approval_broker = &*broker;
  successor->context.approval_token = &token;
  fixture.poll();
  fixture.finish_holder();

  require_success(holder);
  require_success(successor);
  REQUIRE(waited);
  REQUIRE(cancelled_while_held);
  REQUIRE_FALSE(cancelled->failure);
  REQUIRE_FALSE(cancelled->result->has_value());
  REQUIRE(cancelled->result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE_FALSE(cancelled->context.resolved_path.has_value());
  REQUIRE(fixture.executed.size() == 2);
  REQUIRE(fixture.completed.size() == 3);
  REQUIRE(fixture.contents("shared.txt") == "approved");
  REQUIRE(fixture.audit.events().size() == 2);
}

TEST_CASE("Tool admission releases a refused or unaudited operation without effects",
          "[integration][tool][admission][permission][audit]") {
  Admission fixture;
  FailedAudit failed_audit;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string original = R"({"path":"original.txt","content":"unused"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = original,
                                                         .identity = "refused",
                                                         .ttl = 60s,
                                                         .replay_max = 1},
                               core::time::now_utc());
  fixture.rewrite("refused", R"({"path":"final.txt","content":"refused"})");
  auto mode = permission::Mode::default_;
  bool unmatched = true;
  bool replay = false;
  permission::AuditSink* audit_sink = &fixture.audit;
  auto expected_kind = core::ErrorKind::permission_denied;
  SECTION("deny") {
    mode = permission::Mode::strict;
  }
  SECTION("approval required") {}
  SECTION("approval cannot authorize a different final input") {
    replay = true;
  }
  SECTION("audit failure") {
    audit_sink = &failed_audit;
    unmatched = false;
    expected_kind = core::ErrorKind::internal;
  }
  auto refused = fixture.start("FileWrite", original, "refused", audit_sink);
  refused->context.mode = mode;
  if (unmatched) {
    refused->context.rules = {};
  }
  if (replay) {
    refused->context.approval_broker = &*broker;
    refused->context.approval_token = &token;
  }
  fixture.poll();
  const bool no_effect = !std::filesystem::exists(fixture.root / "final.txt") && fixture.executed.empty();
  auto recovered = fixture.start("FileWrite", R"({"path":"final.txt","content":"recovered"})", "recovered");
  fixture.poll();

  REQUIRE(no_effect);
  REQUIRE_FALSE(refused->failure);
  REQUIRE(refused->result.has_value());
  REQUIRE_FALSE(refused->result->has_value());
  REQUIRE(refused->result->error().kind() == expected_kind);
  require_success(recovered);
  REQUIRE(fixture.contents("final.txt") == "recovered");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "original.txt"));
}

TEST_CASE("Tool admission does not extend approval expiry while waiting for a path",
          "[integration][tool][admission][approval]") {
  Admission fixture;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  const std::string input = R"({"path":"shared.txt","content":"expired"})";
  auto token = broker->approve(permission::ApprovalGrant{.tool_name = "FileWrite",
                                                         .input = input,
                                                         .identity = "expiring",
                                                         .ttl = 1s,
                                                         .replay_max = 1},
                               core::Time::epoch());
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  auto expiring = fixture.start("FileWrite", input, "expiring");
  expiring->context.rules = {};
  expiring->context.approval_broker = &*broker;
  expiring->context.approval_token = &token;
  expiring->context.now = core::Time{token.expires_at.to_system_time_point() - 50ms};
  fixture.poll();
  fixture.io.restart();
  fixture.io.run_for(80ms);
  fixture.finish_holder();

  require_success(holder);
  REQUIRE_FALSE(expiring->failure);
  REQUIRE(expiring->result.has_value());
  REQUIRE_FALSE(expiring->result->has_value());
  REQUIRE(std::ranges::any_of(expiring->result->error().context(),
                              [](const auto& entry) { return entry.first == "reason" && entry.second == "expired"; }));
  REQUIRE(fixture.contents("shared.txt") == "holder");
  REQUIRE(fixture.executed.size() == 1);
}

TEST_CASE("File preparation rejects invalid input before approval or authority",
          "[integration][tool][preparation][approval]") {
  Admission fixture;
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  bool replay = false;
  bool workspace = true;
  SECTION("approval consumer") {}
  SECTION("existing replay grant") {
    replay = true;
  }
  SECTION("no workspace") {
    workspace = false;
  }
  std::ofstream{fixture.root / "shared.txt"} << "original";

  for (const auto name : {"FileRead", "FileWrite", "FileEdit"}) {
    auto base = nlohmann::json{{"path", "shared.txt"}};
    if (name == std::string_view{"FileWrite"}) {
      base["content"] = "changed";
    } else if (name == std::string_view{"FileEdit"}) {
      base["old_string"] = "original";
      base["new_string"] = "changed";
    }
    std::vector<std::string> invalid{"{not-json", "[]", "{}"};
    auto missing = base;
    missing.erase("path");
    invalid.push_back(missing.dump());
    auto changes = std::vector<nlohmann::json>{
        {{"path", 42}}, {{"path", ""}}, {{"path", std::string{"bad\0path", 8}}},
        {{"max_bytes", 0}}, {{"max_bytes", -1}}, {{"max_bytes", 1.5}}, {{"max_bytes", "4"}},
        {{"max_bytes", 16777217}}, {{"max_bytes", 18446744073709551615ULL}}, {{"unexpected", true}}};
    if (name == std::string_view{"FileRead"}) {
      for (auto change : std::vector<nlohmann::json>{
               {{"offset", 0}}, {{"offset", -1}}, {{"offset", "1"}}, {{"offset", 1.5}},
               {{"limit", 0}}, {{"limit", -1}}, {{"limit", "1"}}, {{"limit", 1.5}},
               {{"limit", 2001}}, {{"limit", 18446744073709551615ULL}},
               {{"allow_outside_workspace", "yes"}}}) {
        changes.push_back(std::move(change));
      }
    } else if (name == std::string_view{"FileWrite"}) {
      missing = base;
      missing.erase("content");
      invalid.push_back(missing.dump());
      for (auto change : std::vector<nlohmann::json>{
               {{"content", 1}}, {{"mode", 1}}, {{"mode", "unknown"}}, {{"create_parents", "yes"}},
               {{"max_bytes", 1}}, {{"expected_version", false}}, {{"allow_outside_workspace", true}},
               {{"path", "nested/new.txt"}, {"create_parents", true}, {"content", false}}}) {
        changes.push_back(std::move(change));
      }
    } else {
      for (const auto field : {"old_string", "new_string"}) {
        missing = base;
        missing.erase(field);
        invalid.push_back(missing.dump());
      }
      for (auto change : std::vector<nlohmann::json>{
               {{"old_string", false}}, {{"new_string", false}}, {{"old_string", ""}},
               {{"new_string", "original"}}, {{"replace_all", "yes"}}, {{"expected_version", 3}},
               {{"allow_outside_workspace", true}}}) {
        changes.push_back(std::move(change));
      }
    }
    for (const auto& change : changes) {
      auto input = base;
      input.update(change);
      invalid.push_back(input.dump());
    }

    for (const auto& input : invalid) {
      CAPTURE(name, input);
      auto token = broker->approve(permission::ApprovalGrant{.tool_name = name,
                                                             .input = input,
                                                             .identity = "invalid",
                                                             .ttl = 60s,
                                                             .replay_max = 1},
                                   core::time::now_utc());
      const auto previous = fixture.audit.events().size();
      auto call = fixture.start(name, input, "invalid");
      call->context.rules = {};
      call->context.approval_broker = &*broker;
      call->context.approval_token = replay ? &token : nullptr;
      call->context.workspace = workspace ? &*fixture.workspace : nullptr;
      fixture.poll();

      REQUIRE_FALSE(call->failure);
      REQUIRE(call->result.has_value());
      REQUIRE_FALSE(call->result->has_value());
      REQUIRE(call->result->error().kind() == core::ErrorKind::invalid_argument);
      REQUIRE_FALSE(call->context.resolved_path.has_value());
      REQUIRE(fixture.approval_requests == 0);
      REQUIRE(fixture.executed.empty());
      REQUIRE(fixture.audit.events().size() == previous + 1);
      const auto& event = fixture.audit.events().back();
      REQUIRE(event.outcome == permission::AuditOutcome::deny);
      REQUIRE(event.reason == "invalid_tool_input");
      REQUIRE(event.input_hash == permission::ApprovalAuthority::input_hash(input));
      REQUIRE(fixture.completed.size() == previous + 1);
      REQUIRE_FALSE(fixture.completed.back().succeeded);
      REQUIRE(fixture.completed.back().error_kind == "invalid_argument");
      auto unused = broker->check(token, name, input, "invalid", call->context.now);
      REQUIRE(unused.has_value());
    }
  }
  REQUIRE(fixture.contents("shared.txt") == "original");
  REQUIRE_FALSE(std::filesystem::exists(fixture.root / "nested"));
  auto valid = fixture.start("FileRead", R"({"path":"shared.txt"})", "valid");
  valid->context.rules = {};
  valid->context.approval_broker = &*broker;
  fixture.poll();
  require_success(valid);
  REQUIRE(fixture.approval_requests == 1);
}

TEST_CASE("File preparation rejects rewritten invalid input without waiting for a path",
          "[integration][tool][preparation][lock][hook]") {
  Admission fixture;
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  const std::string invalid = R"({"path":"shared.txt","content":false})";
  fixture.rewrite("invalid", invalid);
  auto call = fixture.start("FileWrite", R"({"path":"shared.txt","content":"unused"})", "invalid");
  fixture.poll();
  const bool finished_while_held = call->result.has_value();
  fixture.finish_holder();

  require_success(holder);
  REQUIRE(finished_while_held);
  REQUIRE_FALSE(call->failure);
  REQUIRE_FALSE(call->result->has_value());
  REQUIRE(call->result->error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE_FALSE(call->context.resolved_path.has_value());
  REQUIRE(fixture.audit.events().back().input_hash == permission::ApprovalAuthority::input_hash(invalid));
  REQUIRE(fixture.executed.size() == 1);
  REQUIRE(fixture.contents("shared.txt") == "holder");
}

TEST_CASE("File preparation executes repaired hook input and options",
          "[integration][tool][preparation][hook]") {
  Admission fixture;
  std::ofstream{fixture.root / "shared.txt"} << "first\nsecond\nfirst\n";
  std::string name;
  std::string input;
  std::string expected;
  SECTION("read range") {
    name = "FileRead";
    input = R"({"path":"shared.txt","offset":2,"limit":1})";
    expected = "second\n";
  }
  SECTION("append") {
    name = "FileWrite";
    input = R"({"path":"shared.txt","content":"last","mode":"append"})";
    expected = "first\nsecond\nfirst\nlast";
  }
  SECTION("parent creation") {
    name = "FileWrite";
    input = R"({"path":"nested/new.txt","content":"created","mode":"fail_if_exists","create_parents":true})";
    expected = "created";
  }
  SECTION("edit every match") {
    name = "FileEdit";
    input = R"({"path":"shared.txt","old_string":"first","new_string":"changed","replace_all":true})";
    expected = "changed\nsecond\nchanged\n";
  }
  fixture.rewrite("repaired", input);
  auto call = fixture.start(name, "{invalid", "repaired");
  fixture.poll();
  require_success(call);
  if (name == "FileRead") {
    REQUIRE(nlohmann::json::parse(*call->result->value().data_json)["text"] == expected);
  } else {
    REQUIRE(fixture.contents(input.contains("nested") ? "nested/new.txt" : "shared.txt") == expected);
  }
  REQUIRE(fixture.executed.size() == 1);
  REQUIRE(fixture.executed.front().input_json == input);
  REQUIRE(fixture.audit.events().front().input_hash == permission::ApprovalAuthority::input_hash(input));
}

TEST_CASE("Prepared custom calls retain arguments while waiting and resolve their declared target",
          "[integration][tool][preparation][lock][ownership]") {
  Admission fixture;
  std::size_t preparations = 0;
  const std::string input = R"({"target":"shared.txt","prefix":"prepared: "})";
  REQUIRE(fixture.registry.add_prepared(
      {.name = "PreparedRead", .description = "Read an explicitly prepared target",
       .input_schema_json = "{}", .required_capabilities = {core::Capability::read_file}},
      [&preparations](std::string_view bytes) -> core::Result<tool::PreparedCall> {
        ++preparations;
        const auto parsed = nlohmann::json::parse(bytes);
        return tool::PreparedCall{
            .path = tool::PathRequest{.path = parsed.at("target").get<std::string>(), .intent = tool::PathIntent::read},
            .execute = [prefix = std::make_unique<std::string>(parsed.at("prefix").get<std::string>())](
                           tool::DispatchContext& ctx) mutable { return read_prepared(std::move(prefix), ctx); },
        };
      }).has_value());
  auto holder = fixture.start("FileWrite", R"({"path":"shared.txt","content":"holder"})", "holder");
  fixture.poll();
  fixture.rewrite("prepared", input);
  auto call = fixture.start("PreparedRead", "{invalid", "prepared");
  fixture.poll();
  const bool waited_without_authority = !call->result && !call->context.resolved_path;
  const auto prepared_before_release = preparations;
  fixture.finish_holder();

  require_success(holder);
  require_success(call);
  REQUIRE(waited_without_authority);
  REQUIRE(prepared_before_release == 1);
  REQUIRE(preparations == 1);
  REQUIRE(call->result->value().text == "prepared: holder");
  REQUIRE(fixture.audit.events().size() == 2);
}

TEST_CASE("Ordinary handlers retain state and do not acquire authority from a built-in name",
          "[integration][tool][preparation]") {
  Admission fixture;
  REQUIRE(fixture.registry.remove("FileRead").has_value());
  REQUIRE(fixture.registry.add(
      {.name = "FileRead", .description = "Ordinary stateful handler",
       .input_schema_json = "{}", .required_capabilities = {core::Capability::read_file}},
      [calls = 0](std::string_view, tool::DispatchContext&) mutable -> async::Awaitable<core::Result<tool::Output>> {
        co_return tool::Output{.text = std::to_string(++calls)};
      }).has_value());
  for (int i = 1; i <= 2; ++i) {
    auto call = fixture.start("FileRead", R"({"path":"../outside.txt"})", "ordinary");
    fixture.poll();
    require_success(call);
    REQUIRE(call->result->value().text == std::to_string(i));
    REQUIRE_FALSE(call->context.resolved_path.has_value());
  }
}
