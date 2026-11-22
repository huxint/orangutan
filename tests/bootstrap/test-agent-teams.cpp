#include "team-fixture.hpp"

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

using namespace team_tests;

TEST_CASE("New owners can reclaim stopped boards without evicting live teams", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        const auto first_owner = bootstrap::task_owner(f.options);
        for (int index = 0; index < 16; ++index) {
          REQUIRE(f.command(tool::TeamCreateRequest{"retain a board", {{"member", "inspect", {}}}}));
          if (index == 0)
            REQUIRE(f.command(tool::TeamStopRequest{}));
          auto next = core::generate_turn_id();
          REQUIRE(next.has_value());
          f.options.session_id = *next;
        }
        REQUIRE(f.command(tool::TeamCreateRequest{"replace stopped board", {{"member", "inspect", {}}}}));
        CHECK_FALSE(f.tasks->get_team(first_owner));
        auto next = core::generate_turn_id();
        REQUIRE(next.has_value());
        f.options.session_id = *next;
        auto full = f.command(tool::TeamCreateRequest{"cannot evict live boards", {{"member", "inspect", {}}}});
        REQUIRE_FALSE(full.has_value());
        CHECK(full.error().kind() == core::ErrorKind::mailbox_overflowed);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Waiting team tasks reserve records against unrelated background admission", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor(), {.max_running = 1, .max_queued = 1, .max_records = 2, .enable_teams = true}};
        REQUIRE(f.command(tool::TeamCreateRequest{"dependent work", {{"one", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"first", "one", "inspect", {}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"second", "one", "use the finding", {"first"}}));
        const auto first = co_await f.settled("first");
        CHECK(first.state == bootstrap::TeamTaskState::in_review);
        auto context = tool::DispatchContext::for_now(f.executor,
                                                      f.rules,
                                                      f.assembly->audit_sink(),
                                                      f.options.scope_key,
                                                      f.options.agent_key,
                                                      f.options.identity);
        auto unrelated = f.tasks->start(f.options, {.prompt = "unrelated", .background = true}, context);
        REQUIRE_FALSE(unrelated.has_value());
        CHECK(unrelated.error().kind() == core::ErrorKind::mailbox_overflowed);
        CHECK_FALSE(f.command(tool::TeamTaskCreateRequest{"overflow", "one", "another task", {}}));
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"first", 1, tool::TeamTaskAction::accept, "checked"}));
        const auto second = co_await f.settled("second");
        CHECK(second.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.provider.requests.size() == 2);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Team native tools create arbitrary roles and unlock only reviewed dependencies", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        auto created = co_await f.native("TeamCreate",
                                         {{"goal", "trace a protocol and test its edges"},
                                          {"members",
                                           Json::array({{{"name", "protocol-archeologist"},
                                                         {"instructions", "reconstruct the wire contract"},
                                                         {"tools", Json::array()}}})}});
        REQUIRE_FALSE(created.is_error);
        auto added = co_await f.native(
            "TeamMemberAdd",
            {{"name", "edge-hunter"}, {"instructions", "investigate edge cases"}, {"tools", Json::array()}});
        REQUIRE_FALSE(added.is_error);
        REQUIRE(f.board().members.size() == 2);
        auto a = co_await f.native(
            "TeamTaskCreate",
            {{"key", "protocol"}, {"member", "protocol-archeologist"}, {"prompt", "map packet framing"}});
        REQUIRE_FALSE(a.is_error);
        auto b = co_await f.native("TeamTaskCreate",
                                   {{"key", "edges"}, {"member", "edge-hunter"}, {"prompt", "find malformed inputs"}});
        REQUIRE_FALSE(b.is_error);
        auto c = co_await f.native("TeamTaskCreate",
                                   {{"key", "combine"},
                                    {"member", "edge-hunter"},
                                    {"prompt", "verify framing against malformed inputs"},
                                    {"depends_on", {"protocol", "edges"}}});
        REQUIRE_FALSE(c.is_error);
        const auto first = co_await f.settled("protocol");
        const auto second = co_await f.settled("edges");
        CHECK(first.state == bootstrap::TeamTaskState::in_review);
        CHECK(second.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.task("combine").state == bootstrap::TeamTaskState::waiting);
        auto early = f.command(tool::TeamTaskUpdateRequest{"combine", 1, tool::TeamTaskAction::accept, "unverified"});
        CHECK_FALSE(early.has_value());

        auto accepted = co_await f.native("TeamTaskUpdate",
                                          {{"team_id", f.board().team_id},
                                           {"key", "protocol"},
                                           {"attempt", 1},
                                           {"action", "accept"},
                                           {"feedback", "checked framing"}});
        REQUIRE_FALSE(accepted.is_error);
        CHECK(f.task("combine").state == bootstrap::TeamTaskState::waiting);
        REQUIRE(
            f.command(tool::TeamTaskUpdateRequest{"edges", 1, tool::TeamTaskAction::accept, "checked edge evidence"}));
        const auto joined = co_await f.settled("combine");
        CHECK(joined.state == bootstrap::TeamTaskState::in_review);
        const auto request = std::ranges::find_last_if(f.provider.requests, [](const auto& value) {
          return assignment(value).value("task", "") == "combine";
        });
        REQUIRE_FALSE(request.empty());
        const auto context = assignment(request.front());
        REQUIRE(context.at("dependencies").size() == 2);
        CHECK(context.at("dependencies")[0].at("report") == "protocol report 1");
        CHECK(context.at("dependencies")[1].at("report") == "edges report 1");

        const auto completion = f.tasks->next_completion(bootstrap::task_owner(f.options));
        REQUIRE(completion.has_value());
        auto delivered = co_await f.session->run_completion({*completion});
        REQUIRE(delivered.has_value());
        CHECK(delivered->has_value());
        CHECK(f.provider.completions == 1);
        auto duplicate = co_await f.session->run_completion({*completion});
        REQUIRE(duplicate.has_value());
        CHECK_FALSE(duplicate->has_value());
        CHECK(f.provider.completions == 1);
        CHECK(f.task("combine").state == bootstrap::TeamTaskState::in_review);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Team corrections reuse sessions and cache prefixes while stale acceptance fails",
          "[bootstrap][teams][cache]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        REQUIRE(
            f.command(tool::TeamCreateRequest{"investigate a boundary", {{"boundary-scout", "inspect the edge", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"edge", "boundary-scout", "inspect initial edge", {}}));
        const auto initial = co_await f.settled("edge");
        const auto identity = f.board().members.front();
        REQUIRE(f.command(
            tool::TeamTaskUpdateRequest{"edge", 1, tool::TeamTaskAction::revise, "also check an empty input"}));
        auto stale = f.command(tool::TeamTaskUpdateRequest{"edge", 1, tool::TeamTaskAction::accept, "old evidence"});
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().kind() == core::ErrorKind::conflict);
        const auto corrected = co_await f.settled("edge");
        CHECK(corrected.attempt == 2);
        CHECK(initial.run_id != corrected.run_id);
        CHECK(f.board().members.front().session_id == identity.session_id);
        REQUIRE(f.provider.requests.size() == 2);
        const auto& first = f.provider.requests.front();
        const auto& next = f.provider.requests.back();
        CHECK(first.system_prompt == next.system_prompt);
        CHECK(first.cache == next.cache);
        CHECK(first.tools == next.tools);
        CHECK_FALSE(first.system_prompt->contains("private parent context"));
        CHECK_FALSE(first.system_prompt->contains("boundary-scout"));
        CHECK(first.messages.size() == 1);
        REQUIRE(next.messages.size() == 3);
        CHECK(message_text(next.messages[1]) == "edge report 1");
        CHECK(assignment(next).at("feedback") == "also check an empty input");
        CHECK_FALSE(assignment(next).contains("goal"));
        auto stored = co_await f.assembly->session_store()->load({identity.session_id}, {identity.agent_key});
        REQUIRE(stored.has_value());
        CHECK(stored->size() == 4);
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"edge", 2, tool::TeamTaskAction::revise, "check final edge"}));
        const auto final = co_await f.settled("edge");
        CHECK(final.attempt == 3);
        CHECK_FALSE(f.command(tool::TeamTaskUpdateRequest{"edge", 3, tool::TeamTaskAction::revise, "another round"}));
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"edge", 3, tool::TeamTaskAction::accept, "verified artifact"}));
        CHECK_FALSE(f.command(tool::TeamTaskUpdateRequest{"edge", 3, tool::TeamTaskAction::cancel, {}}));
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Team membership and task ownership stay scoped and bounded", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        f.options.max_child_runs = 1;
        REQUIRE(f.command(tool::TeamCreateRequest{"bounded work", {{"any-name", "inspect", {}}}}));
        CHECK_FALSE(f.command(tool::TeamMemberAddRequest{{"another-name", "inspect", {}}}));
        auto foreign = bootstrap::task_owner(f.options);
        foreign.scope_key = "foreign";
        auto hidden = f.tasks->get_team(foreign);
        REQUIRE_FALSE(hidden.has_value());
        CHECK(hidden.error().kind() == core::ErrorKind::not_found);
        CHECK_FALSE(f.command(tool::TeamTaskCreateRequest{"a", "any-name", "inspect", {"missing"}}));
        CHECK_FALSE(f.command(tool::TeamTaskCreateRequest{"a", "any-name", "inspect", {"a"}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"a", "any-name", "inspect", {}}));
        CHECK_FALSE(f.command(tool::TeamTaskCreateRequest{"a", "any-name", "duplicate", {}}));
        for (int index = 1; index < 32; ++index)
          REQUIRE(f.command(tool::TeamTaskCreateRequest{std::to_string(index), "any-name", "inspect", {"a"}}));
        CHECK_FALSE(f.command(tool::TeamTaskCreateRequest{"overflow", "any-name", "inspect", {}}));
        REQUIRE(f.command(tool::TeamStopRequest{}));
        co_await f.shutdown();
        CHECK(std::ranges::all_of(f.board().tasks,
                                  [](const auto& task) { return task.state == bootstrap::TeamTaskState::cancelled; }));
      },
      5s);
}

TEST_CASE("Team tasks share global capacity and stop waits for borrowed work", "[bootstrap][teams][cancellation]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor(),
                  {.max_running = 1, .max_queued = 0, .automatic_delivery = true, .enable_teams = true}};
        async::Channel<int> started{io.get_executor(), 1}, release{io.get_executor(), 1};
        f.provider.specialist = [&started, &release](provider::Request,
                                                     Json) -> async::Awaitable<core::Result<provider::Response>> {
          REQUIRE(started.try_send(1));
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
          auto value = co_await release.receive();
          REQUIRE(value.has_value());
          co_return answer("late result");
        };
        REQUIRE(f.command(tool::TeamCreateRequest{"bounded work", {{"a", "inspect", {}}, {"b", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"a1", "a", "inspect", {}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"b1", "b", "inspect", {}}));
        auto active = co_await started.receive();
        REQUIRE(active.has_value());
        CHECK(f.task("b1").state == bootstrap::TeamTaskState::waiting);
        CHECK(f.provider.requests.size() == 1);
        REQUIRE(f.command(tool::TeamStopRequest{}));
        CHECK(f.task("a1").state == bootstrap::TeamTaskState::cancelling);
        CHECK(f.task("b1").state == bootstrap::TeamTaskState::cancelled);
        CHECK_FALSE(f.command(tool::TeamCreateRequest{"replacement", {{"c", "inspect", {}}}}));
        bool joined = false;
        asio::co_spawn(io, f.tasks->shutdown(), [&joined](std::exception_ptr error, core::Result<void> result) {
          REQUIRE_FALSE(error);
          REQUIRE(result.has_value());
          joined = true;
        });
        auto yielded = co_await async::sleep_for(io.get_executor(), 1ms);
        REQUIRE(yielded.has_value());
        CHECK_FALSE(joined);
        REQUIRE(release.try_send(1));
        while (!joined) {
          auto waited = co_await async::sleep_for(io.get_executor(), 1ms);
          REQUIRE(waited.has_value());
        }
        CHECK(f.task("a1").state == bootstrap::TeamTaskState::cancelled);
        CHECK(f.provider.requests.size() == 1);
      },
      5s);
}

TEST_CASE("One specialist serializes turns while independent members make progress", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        async::Channel<int> started{io.get_executor(), 1}, release{io.get_executor(), 1};
        f.provider.specialist = [&started, &release](provider::Request,
                                                     Json task) -> async::Awaitable<core::Result<provider::Response>> {
          if (task.at("task") == "first") {
            REQUIRE(started.try_send(1));
            auto ready = co_await release.receive();
            REQUIRE(ready.has_value());
          }
          co_return answer("finished");
        };
        REQUIRE(f.command(tool::TeamCreateRequest{"separable work", {{"one", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"first", "one", "first", {}}));
        auto running = co_await started.receive();
        REQUIRE(running.has_value());
        REQUIRE(f.command(tool::TeamMemberAddRequest{{"two", "inspect", {}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"second", "one", "second", {}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"independent", "two", "separate", {}}));
        const auto independent = co_await f.settled("independent");
        CHECK(independent.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.task("second").state == bootstrap::TeamTaskState::waiting);
        CHECK(f.provider.requests.size() == 2);
        CHECK(f.provider.requests[0].cache == f.provider.requests[1].cache);
        CHECK(f.provider.requests[0].system_prompt == f.provider.requests[1].system_prompt);
        REQUIRE(release.try_send(1));
        const auto second = co_await f.settled("second");
        CHECK(second.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.provider.requests.size() == 3);
        CHECK(f.provider.requests.back().messages.size() == 3);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Replaced teams reject old reviews and retain paged evidence until replacement", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        f.provider.specialist = [](provider::Request, Json) -> async::Awaitable<core::Result<provider::Response>> {
          co_return answer("界abcdef");
        };
        REQUIRE(f.command(tool::TeamCreateRequest{"old goal", {{"member", "inspect", {}}}}));
        const auto old_id = f.board().team_id;
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"same-key", "member", "inspect", {}}));
        const auto reported = co_await f.settled("same-key");
        CHECK(reported.state == bootstrap::TeamTaskState::in_review);
        CHECK_FALSE(f.command(tool::TeamGetRequest{"same-key", 1, 3}));
        CHECK_FALSE(f.command(tool::TeamGetRequest{"same-key", 0, 2}));
        auto page = f.command(tool::TeamGetRequest{"same-key", 0, 3});
        REQUIRE(page.has_value());
        const auto data = Json::parse(*page->data_json);
        CHECK(data.at("tasks")[0].at("report") == "界");
        CHECK(data.at("tasks")[0].at("next_offset") == 3);
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"same-key", 1, tool::TeamTaskAction::accept, "checked"}));
        REQUIRE(f.command(tool::TeamStopRequest{}));
        CHECK_FALSE(f.tasks->next_completion(bootstrap::task_owner(f.options)));
        CHECK(f.task("same-key").report == "界abcdef");
        REQUIRE(f.command(tool::TeamCreateRequest{"new goal", {{"member", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"same-key", "member", "new work", {}}));
        const auto next = co_await f.settled("same-key");
        CHECK(next.attempt == 1);
        auto stale =
            f.command(tool::TeamTaskUpdateRequest{"same-key", 1, tool::TeamTaskAction::accept, "old review", old_id});
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().kind() == core::ErrorKind::conflict);
        CHECK(f.task("same-key").state == bootstrap::TeamTaskState::in_review);
        co_await f.shutdown();
      },
      5s);
}
