#include "../../apps/telegram/commands.hpp"
#include "../../apps/telegram/host.hpp"
#include "../test-helpers/run_async.hpp"

#include <filesystem>
#include <stdexcept>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/post.hpp>
#include <oran/async/sleep.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/session_status.hpp>
#include <oran/hook/bus.hpp>
#include <oran/memory/session.hpp>
#include <oran/provider/system.hpp>
#include <oran/storage/trace_repository.hpp>

namespace {
using namespace orangutan;
using core::Result;
using telegram_host::Json;
using telegram_host::PollMethod;

struct StateDirectory {
  std::filesystem::path path;
  StateDirectory() {
    auto id = core::generate_turn_id();
    REQUIRE(id);
    path = std::filesystem::temp_directory_path() / ("oran-telegram-" + core::format_turn_id_hex(*id));
  }
  ~StateDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

channel::Response response(Json result) {
  return {200, Json{{"ok", true}, {"result", std::move(result)}}.dump(), {}};
}
Json update(int id, int user = 42, std::string kind = "private") {
  return {{"update_id", id},
          {"message",
           {{"message_id", id},
            {"text", "hello"},
            {"from", {{"id", user}, {"is_bot", false}}},
            {"chat", {{"id", user}, {"type", std::move(kind)}}}}}};
}
telegram_host::Api api(std::vector<Json> updates, std::vector<std::int64_t>& offsets, bool webhook = false) {
  return [updates = std::move(updates), &offsets, webhook, index = std::size_t{}](
             PollMethod method,
             std::string payload) mutable -> async::Awaitable<Result<channel::Response>> {
    if (method == PollMethod::getMe)
      co_return response(Json{{"is_bot", true}, {"id", 123}, {"username", "fixture_bot"}});
    if (method == PollMethod::getWebhookInfo)
      co_return response(Json{{"url", webhook ? "https://example.test/hook" : ""}});
    if (method == PollMethod::setMyCommands) {
      const auto menu = Json::parse(payload);
      CHECK(menu.at("scope") == Json{{"type", "chat"}, {"chat_id", "42"}});
      co_return response(true);
    }
    REQUIRE(method == PollMethod::getUpdates);
    auto body = Json::parse(payload);
    CHECK(body.at("limit") == 1);
    offsets.push_back(body.at("offset").get<std::int64_t>());
    REQUIRE(index < updates.size());
    co_return response(Json::array({updates[index++]}));
  };
}
}  // namespace

TEST_CASE("Telegram prioritizes users and journals automatic completion without advancing updates",
          "[telegram-host][background]") {
  bool ambiguous = false;
  SECTION("successful notification") {}
  SECTION("ambiguous notification") {
    ambiguous = true;
  }
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  const std::string task_id = "00000000000000000000000000000001";
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int polls = 0, turns = 0, completions = 0, sends = 0;
    std::vector<std::int64_t> offsets;
    auto ordinary = api({update(10)}, offsets);
    telegram_host::Api polling = [&](PollMethod method,
                                     std::string payload) -> async::Awaitable<Result<channel::Response>> {
      if (method != PollMethod::getUpdates)
        co_return co_await ordinary(method, std::move(payload));
      ++polls;
      if (polls == 1) {
        CHECK(Json::parse(payload).at("timeout") == 2);
        co_return co_await ordinary(method, std::move(payload));
      }
      CHECK(Json::parse(payload).at("offset") == 11);
      if (polls == 2)
        co_return response(Json::array());
      co_return std::unexpected(core::Error::cancelled());
    };
    telegram_host::BackgroundPort background;
    background.active = [](core::TurnId) {
      return true;
    };
    background.next = [&](core::TurnId id) -> std::optional<std::string> {
      CHECK(id == state->session);
      return completions == 0 ? std::optional{task_id} : std::nullopt;
    };
    background.complete = [&](core::TurnId id, std::string task) -> async::Awaitable<Result<std::string>> {
      ++completions;
      CHECK(turns == 1);
      CHECK(id == state->session);
      CHECK(task == task_id);
      auto bytes = directory->read("state.json", 2 * 1024 * 1024);
      REQUIRE(bytes);
      const auto journal = Json::parse(**bytes);
      CHECK(journal.at("pending").at("task_id") == task_id);
      CHECK(journal.at("pending").at("answer").is_null());
      co_return "reviewed result";
    };
    auto result = co_await telegram_host::run(
        {.user = "42"},
        polling,
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessage") {
            ++sends;
            const auto body = Json::parse(request.body);
            auto bytes = directory->read("state.json", 2 * 1024 * 1024);
            REQUIRE(bytes);
            const auto journal = Json::parse(**bytes);
            CHECK(journal.at("pending").at("send_inflight") == true);
            if (sends == 2) {
              CHECK(body.at("text") == "reviewed result");
              CHECK_FALSE(body.contains("reply_parameters"));
              CHECK(journal.at("next_update") == 11);
              CHECK(journal.at("pending").at("task_id") == task_id);
              CHECK(journal.at("pending").at("answer") == "reviewed result");
              if (ambiguous)
                co_return std::unexpected(core::Error::network("ambiguous send"));
            }
          }
          co_return response(Json{{"message_id", 100}});
        },
        [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
          ++turns;
          CHECK(message.text == "hello");
          CHECK(completions == 0);
          co_return "foreground answer";
        },
        hooks,
        *directory,
        *state,
        io.get_executor(),
        nullptr,
        {},
        std::move(background));
    REQUIRE_FALSE(result.has_value());
    CHECK(turns == 1);
    CHECK(completions == 1);
    CHECK(sends == 2);
    CHECK(state->next_update == 11);
    if (ambiguous) {
      REQUIRE(state->pending);
      CHECK(state->pending->task_id == task_id);
      CHECK(state->pending->send_inflight);
      CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
      REQUIRE(telegram_host::acknowledge_pending(*directory, 11));
      auto archive = directory->read("handled-task-" + task_id + ".json", 2 * 1024 * 1024);
      REQUIRE(archive);
      REQUIRE(archive->has_value());
      CHECK(Json::parse(**archive).at("pending").at("answer") == "reviewed result");
    } else {
      CHECK(result.error().kind() == core::ErrorKind::cancelled);
      CHECK_FALSE(state->pending);
    }
    auto reopened = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reopened);
    CHECK(reopened->next_update == 11);
    CHECK_FALSE(reopened->pending);
  });
}

TEST_CASE("Telegram task commands bypass the model and new cancels the old session", "[telegram-host][background]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  const auto previous = state->session;
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int turns = 0, lists = 0, stops = 0, index = 10;
    std::vector<std::int64_t> offsets;
    for (const auto* command : {"/tasks", "/stop", "/new"}) {
      auto item = update(index++);
      item["message"]["text"] = command;
      telegram_host::BackgroundPort background;
      background.control = [&](core::TurnId id, telegram_host::TaskCommand operation) -> Result<std::string> {
        CHECK(id == previous);
        CHECK(state->session == previous);
        if (operation == telegram_host::TaskCommand::list)
          ++lists;
        else
          ++stops;
        return "task status";
      };
      auto result = co_await telegram_host::run(
          {.user = "42", .once = true},
          api({item}, offsets),
          [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
            co_return response(Json{{"message_id", 100}});
          },
          [&](channel::Message) -> async::Awaitable<Result<std::string>> {
            ++turns;
            co_return "model";
          },
          hooks,
          *directory,
          *state,
          io.get_executor(),
          nullptr,
          {},
          std::move(background));
      REQUIRE(result);
    }
    CHECK(turns == 0);
    CHECK(lists == 1);
    CHECK(stops == 2);
    CHECK(state->session != previous);
  });
}

TEST_CASE("Telegram parses standalone commands without interpreting paths or quoted prose", "[telegram-host]") {
  auto command = telegram_host::parse_command(" \n/NEW@Fixture_Bot \t", "fixture_bot");
  REQUIRE(command);
  CHECK(command->name == "new");
  CHECK(command->addressed_here);
  CHECK_FALSE(command->has_arguments);
  command = telegram_host::parse_command("/new follow-up", "fixture_bot");
  REQUIRE(command);
  CHECK(command->has_arguments);
  command = telegram_host::parse_command("/status@another_bot", "fixture_bot");
  REQUIRE(command);
  CHECK_FALSE(command->addressed_here);
  for (const auto* text : {"请解释 /new", "> /new", "`/new`", "/home/user/file.cpp", "普通文本", "/"})
    CHECK_FALSE(telegram_host::parse_command(text, "fixture_bot"));
}

TEST_CASE("Telegram new session is durable before delivery and local commands bypass the model", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  const auto previous = state->session;
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    int turns = 0, reads = 0;
    std::vector<std::string> answers;
    channel::RunTurn turn = [&](channel::Message) -> async::Awaitable<Result<std::string>> {
      ++turns;
      co_return "model";
    };
    channel::Transport send = [&](channel::Conversation,
                                  channel::Request request) -> async::Awaitable<Result<channel::Response>> {
      if (request.path == "/sendMessage") {
        answers.push_back(Json::parse(request.body).at("text").get<std::string>());
        auto saved = directory->read("state.json", 2 * 1024 * 1024);
        REQUIRE(saved);
        CHECK(Json::parse(**saved).at("session") == core::format_turn_id_hex(state->session));
        CHECK_FALSE(Json::parse(**saved).at("pending").at("answer").is_null());
      }
      co_return response(Json{{"message_id", 100}});
    };
    int id = 10;
    for (const auto* command :
         {"/help", "/start private-payload", "/whoami", "/status", "/new arg", "/new@fixture_bot"}) {
      auto item = update(id++);
      item["message"]["text"] = command;
      auto normal_api = api({item}, offsets);
      auto menu_may_fail = [&](PollMethod method, std::string payload) -> async::Awaitable<Result<channel::Response>> {
        if (method == PollMethod::setMyCommands && std::string_view{command} == "/help")
          co_return std::unexpected(core::Error::network("private transport detail"));
        co_return co_await normal_api(method, std::move(payload));
      };
      auto result = co_await telegram_host::run({.user = "42", .once = true},
                                                menu_may_fail,
                                                send,
                                                turn,
                                                hooks,
                                                *directory,
                                                *state,
                                                io.get_executor(),
                                                nullptr,
                                                [&](core::TurnId session) -> async::Awaitable<Result<std::string>> {
                                                  ++reads;
                                                  CHECK(session == state->session);
                                                  co_return "fixture status";
                                                });
      REQUIRE(result);
      if (std::string_view{command} != "/new@fixture_bot")
        CHECK(state->session == previous);
    }
    CHECK(turns == 0);
    CHECK(reads == 1);
    REQUIRE(answers.size() == 6);
    CHECK(answers[0].contains("/new"));
    CHECK(answers[2].contains("42"));
    CHECK(answers[3].contains("fixture status"));
    CHECK(answers[4].contains("不附带参数"));
    CHECK(state->session != previous);
    auto reopened = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reopened);
    CHECK(reopened->session == state->session);
    CHECK_FALSE(reopened->pending);
  });
}

TEST_CASE("Telegram new never executes for another sender, bot, or image caption", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  const auto previous = state->session;
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto foreign_user = update(10, 9), foreign_bot = update(11), caption = update(12);
    foreign_user["message"]["text"] = "/new";
    foreign_bot["message"]["text"] = "/new@another_bot";
    caption["message"].erase("text");
    caption["message"]["caption"] = "/new";
    caption["message"]["photo"] = Json::array({{{"file_id", "photo"}, {"width", 10}, {"height", 10}}});
    int turns = 0;
    auto result = co_await telegram_host::run(
        {.user = "42", .once = true},
        api({foreign_user, foreign_bot, caption}, offsets),
        [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
          co_return response(Json{{"message_id", 100}});
        },
        [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
          ++turns;
          CHECK(message.image.has_value());
          co_return "image caption";
        },
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE(result);
    CHECK(turns == 1);
    CHECK(state->session == previous);
    CHECK(offsets == std::vector<std::int64_t>{0, 11, 12});
  });
}

TEST_CASE("Telegram keeps the new session identity when its acknowledgment delivery is ambiguous", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  const auto previous = state->session;
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto item = update(10);
    item["message"]["text"] = "/new";
    auto result = co_await telegram_host::run(
        {.user = "42", .once = true},
        api({item}, offsets),
        [](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessage")
            co_return std::unexpected(core::Error::network("ambiguous send"));
          co_return response(true);
        },
        {},
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(result);
    CHECK(state->session != previous);
    const auto rotated = state->session;
    CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
    REQUIRE(telegram_host::acknowledge_pending(*directory, 10));
    auto reopened = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reopened);
    CHECK(reopened->session == rotated);
  });
}

TEST_CASE("Telegram status reads scoped persisted facts and leaves earlier sessions intact", "[telegram-host]") {
  StateDirectory temp;
  std::filesystem::create_directories(temp.path);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = bootstrap::RuntimeAssembly::build(temp.path.string(), io.get_executor());
    REQUIRE(assembly);
    auto old_id = core::generate_turn_id(), new_id = core::generate_turn_id(), turn_id = core::generate_turn_id();
    REQUIRE(old_id);
    REQUIRE(new_id);
    REQUIRE(turn_id);
    auto* store = assembly->session_store();
    auto saved = co_await store->append({core::format_turn_id_hex(*old_id)},
                                        {"telegram"},
                                        core::Message::user_text("private content"));
    REQUIRE(saved);
    auto traced = co_await assembly->trace_repository()->append_turn({.turn_id = *turn_id,
                                                                      .session_id = *old_id,
                                                                      .agent_key = "telegram",
                                                                      .origin = "telegram",
                                                                      .route_profile = "test",
                                                                      .route_model = "served-model",
                                                                      .started_at_ns = 1,
                                                                      .finished_at_ns = 2,
                                                                      .stop_reason = "end_turn",
                                                                      .cache_read_tokens = 8,
                                                                      .input_tokens = 11,
                                                                      .output_tokens = 7});
    REQUIRE(traced);
    provider::Route route{
        .primary = {.profile = "test", .model = "configured-model", .thinking_budget = {}, .cache = {}},
        .fallbacks = {}};
    auto old_facts = co_await bootstrap::inspect_session(*assembly, *old_id, "telegram", io.get_executor());
    REQUIRE(old_facts);
    const auto old_status = telegram_host::format_session_status(*old_facts, route.primary.model);

    CHECK(old_status.contains("已保存消息：1 条"));
    CHECK(old_status.contains("served-model"));
    CHECK(old_status.contains("输入 11 / 输出 7"));
    CHECK_FALSE(old_status.contains("private content"));
    auto new_facts = co_await bootstrap::inspect_session(*assembly, *new_id, "telegram", io.get_executor());
    REQUIRE(new_facts);
    const auto new_status = telegram_host::format_session_status(*new_facts, route.primary.model);

    CHECK(new_status.contains("已保存消息：0 条"));
    CHECK(new_status.contains("暂无记录"));
    auto original = co_await store->load({core::format_turn_id_hex(*old_id)}, {"telegram"});
    REQUIRE(original);
    REQUIRE(original->size() == 1);
    CHECK(original->front().blocks == core::Message::user_text("private content").blocks);
  });
}

TEST_CASE("Telegram live host filters senders and groups and resumes its saved cursor", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  auto session = state->session;
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int turns = 0, sends = 0;
    channel::RunTurn turn = [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
      CHECK(message.sender == "42");
      auto persisted = directory->read("state.json", 2 * 1024 * 1024);
      REQUIRE(persisted);
      CHECK_FALSE(Json::parse(**persisted).at("pending").is_null());
      ++turns;
      co_return "model answer";
    };
    channel::Transport send = [&](channel::Conversation,
                                  channel::Request request) -> async::Awaitable<Result<channel::Response>> {
      if (request.path == "/sendMessage") {
        ++sends;
        auto persisted = directory->read("state.json", 2 * 1024 * 1024);
        REQUIRE(persisted);
        auto pending = Json::parse(**persisted).at("pending");
        CHECK(pending.at("answer") == "model answer");
        CHECK(pending.at("send_inflight") == true);
        co_return response(Json{{"message_id", 100}});
      }
      co_return response(true);
    };
    std::vector<std::int64_t> offsets;
    auto first_api = api({update(10, 9), update(11, 42, "group"), update(12)}, offsets);
    auto first = co_await telegram_host::run({.user = "42", .once = true},
                                             std::move(first_api),
                                             send,
                                             turn,
                                             hooks,
                                             *directory,
                                             *state,
                                             io.get_executor());
    REQUIRE(first);
    CHECK(turns == 1);
    CHECK(sends == 1);
    CHECK(offsets == std::vector<std::int64_t>{0, 11, 12});
    auto reopened = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reopened);
    CHECK(reopened->session == session);
    CHECK(reopened->next_update == 13);
    CHECK_FALSE(telegram_host::load_state(*directory, "43", "/workspace"));
    CHECK_FALSE(telegram_host::load_state(*directory, "42", "/other-workspace"));
    state = std::move(reopened);
    offsets.clear();
    auto second_api = api({update(13)}, offsets);
    auto second = co_await telegram_host::run({.user = "42", .once = true},
                                              std::move(second_api),
                                              send,
                                              turn,
                                              hooks,
                                              *directory,
                                              *state,
                                              io.get_executor());
    REQUIRE(second);
    CHECK(offsets == std::vector<std::int64_t>{13});
    CHECK(turns == 2);
  });
}

TEST_CASE("Telegram live host preserves ambiguous sends and blocks replay after restart", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto failed_api = api({update(10)}, offsets);
    int sends = 0;
    auto failed = co_await telegram_host::run(
        {.user = "42", .once = true},
        std::move(failed_api),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessage" && ++sends == 2)
            co_return std::unexpected(core::Error::network("PRIVATE_URL"));
          co_return response(Json{{"message_id", 11}});
        },
        [](channel::Message) -> async::Awaitable<Result<std::string>> { co_return std::string(5000, 'x'); },
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(failed);
    CHECK_FALSE(failed.error().message().contains("PRIVATE_URL"));
    auto persisted = directory->read("state.json", 2 * 1024 * 1024);
    REQUIRE(persisted);
    auto journal = Json::parse(**persisted);
    CHECK(journal.at("next_update") == 0);
    CHECK(journal.at("pending").at("update").at("update_id") == 10);
    CHECK(journal.at("pending").at("answer").get<std::string>().size() == 5000);
    CHECK(journal.at("pending").at("send_inflight") == true);
    CHECK(journal.at("pending").at("confirmed_parts") == 1);
    CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
    CHECK_FALSE(telegram_host::acknowledge_pending(*directory, 11));
    REQUIRE(telegram_host::acknowledge_pending(*directory, 10));
    auto reconciled = telegram_host::load_state(*directory, "42", "/workspace");
    REQUIRE(reconciled);
    CHECK(reconciled->next_update == 11);
    auto archived = directory->read("handled-10.json", 2 * 1024 * 1024);
    REQUIRE(archived);
    CHECK(Json::parse(**archived).at("pending").at("answer") == std::string(5000, 'x'));
  });
}

TEST_CASE("Telegram polling retries transient failures but stops competing pollers", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto normal = api({update(10)}, offsets);
    unsigned attempts = 0;
    auto retrying = [&](PollMethod method, std::string payload) -> async::Awaitable<Result<channel::Response>> {
      if (method == PollMethod::getUpdates && ++attempts <= 2)
        co_return std::unexpected(core::Error::upstream("PRIVATE").with_retry_after(std::chrono::milliseconds{0}));
      co_return co_await normal(method, std::move(payload));
    };
    auto result = co_await telegram_host::run(
        {.user = "42", .once = true},
        retrying,
        [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
          co_return response(Json{{"message_id", 11}});
        },
        [](channel::Message) -> async::Awaitable<Result<std::string>> { co_return "answer"; },
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE(result);
    CHECK(attempts == 3);
    CHECK(state->next_update == 11);
    attempts = 0;
    auto conflict = [&](PollMethod method, std::string payload) -> async::Awaitable<Result<channel::Response>> {
      if (method == PollMethod::getUpdates) {
        ++attempts;
        co_return channel::Response{409, R"({"ok":false,"description":"PRIVATE"})", {}};
      }
      co_return co_await normal(method, std::move(payload));
    };
    auto refused = co_await telegram_host::run(
        {.user = "42"},
        conflict,
        [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
          FAIL("unexpected delivery");
          co_return response(true);
        },
        [](channel::Message) -> async::Awaitable<Result<std::string>> { co_return ""; },
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(refused);
    CHECK(refused.error().kind() == core::ErrorKind::conflict);
    CHECK(attempts == 1);
  });
}

TEST_CASE("Telegram recovery rejects a regressing cursor without changing the journal", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  auto json = telegram_host::encode_state(*state);
  json["next_update"] = 20;
  json["pending"] = Json{{"update", update(10)}, {"answer", "saved"}, {"confirmed_parts", 1}, {"send_inflight", true}};
  const auto bytes = json.dump();
  REQUIRE(directory->write("state.json", bytes));
  CHECK_FALSE(telegram_host::acknowledge_pending(*directory, 10));
  auto saved = directory->read("state.json", 2 * 1024 * 1024);
  REQUIRE(saved);
  CHECK(**saved == bytes);
}

TEST_CASE("Telegram live host refuses webhooks and hides transport failures", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::int64_t> offsets;
    auto refused = co_await telegram_host::run({.user = "42"},
                                               api({}, offsets, true),
                                               {},
                                               {},
                                               hooks,
                                               *directory,
                                               *state,
                                               io.get_executor());
    REQUIRE_FALSE(refused);
    CHECK(refused.error().kind() == core::ErrorKind::conflict);
    CHECK(offsets.empty());
    auto hidden = co_await telegram_host::run(
        {.probe = true},
        [](PollMethod, std::string) -> async::Awaitable<Result<channel::Response>> {
          throw std::runtime_error("PRIVATE_TOKEN");
          co_return response(true);
        },
        {},
        {},
        hooks,
        *directory,
        *state,
        io.get_executor());
    REQUIRE_FALSE(hidden);
    CHECK_FALSE(hidden.error().message().contains("PRIVATE_TOKEN"));
  });
  auto limited = telegram_host::api_result(
      {429, R"({"ok":false,"error_code":429,"description":"PRIVATE_TOKEN","parameters":{"retry_after":3}})", {}});
  REQUIRE_FALSE(limited);
  CHECK(limited.error().retry_after() == std::chrono::seconds{3});
  CHECK_FALSE(limited.error().message().contains("PRIVATE_TOKEN"));
}

TEST_CASE("Telegram live host joins cancellation and leaves intake recoverable", "[telegram-host]") {
  StateDirectory temp;
  auto directory = io::PrivateDirectory::open(temp.path.string());
  REQUIRE(directory);
  auto state = telegram_host::load_state(*directory, "42", "/workspace");
  REQUIRE(state);
  asio::io_context io;
  hook::Bus hooks;
  asio::cancellation_signal cancellation;
  std::vector<std::int64_t> offsets;
  bool returned = false, joined = false;
  asio::steady_timer deadline{io, std::chrono::seconds{2}};
  deadline.async_wait([&](asio::error_code error) {
    if (!error)
      io.stop();
  });
  asio::co_spawn(io,
                 telegram_host::run(
                     {.user = "42"},
                     api({update(10)}, offsets),
                     [](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
                       co_return response(true);
                     },
                     [&](channel::Message) -> async::Awaitable<Result<std::string>> {
                       asio::post(io, [&] { cancellation.emit(asio::cancellation_type::all); });
                       auto waited = co_await async::sleep_for(io.get_executor(), std::chrono::seconds{10});
                       joined = true;
                       if (!waited)
                         co_return std::unexpected(waited.error());
                       co_return "unexpected";
                     },
                     hooks,
                     *directory,
                     *state,
                     io.get_executor()),
                 asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error, Result<void> result) {
                   CHECK_FALSE(error);
                   CHECK_FALSE(result);
                   returned = true;
                   deadline.cancel();
                 }));
  io.run();
  CHECK(returned);
  CHECK(joined);
  CHECK_FALSE(telegram_host::load_state(*directory, "42", "/workspace"));
}
