#include "team-fixture.hpp"

#include <oran/storage/trace_repository.hpp>

using namespace team_tests;

TEST_CASE("Waiting corrections do not label the previous report as a new attempt", "[bootstrap][teams][team-review]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor(), {.max_result_bytes = 4, .automatic_delivery = true, .enable_teams = true}};
        async::Channel<int> started{io.get_executor(), 1}, release{io.get_executor(), 1};
        f.provider.specialist = [&started, &release](provider::Request,
                                                     Json task) -> async::Awaitable<core::Result<provider::Response>> {
          if (task.at("task") == "busy") {
            REQUIRE(started.try_send(1));
            auto ready = co_await release.receive();
            REQUIRE(ready.has_value());
          }
          co_return answer("previous report");
        };
        REQUIRE(f.command(tool::TeamCreateRequest{"inspect", {{"member", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"task", "member", "inspect", {}}));
        const auto first = co_await f.settled("task");
        REQUIRE_FALSE(first.report.empty());
        REQUIRE(first.report_truncated);
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"busy", "member", "another task", {}}));
        auto running = co_await started.receive();
        REQUIRE(running.has_value());
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"task", 1, tool::TeamTaskAction::revise, "check again"}));
        auto waiting = f.task("task");
        CHECK(waiting.state == bootstrap::TeamTaskState::waiting);
        CHECK(waiting.attempt == 2);
        CHECK(waiting.report.empty());
        CHECK_FALSE(waiting.report_truncated);
        REQUIRE(release.try_send(1));
        const auto finished = co_await f.settled("task");
        CHECK(finished.state == bootstrap::TeamTaskState::in_review);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("TeamStop suppresses completion from every retained attempt", "[bootstrap][teams][team-review]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        REQUIRE(f.command(tool::TeamCreateRequest{"review corrections", {{"member", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"task", "member", "inspect", {}}));
        const auto first = co_await f.settled("task");
        REQUIRE(f.command(tool::TeamTaskUpdateRequest{"task", 1, tool::TeamTaskAction::revise, "check an edge case"}));
        const auto second = co_await f.settled("task");
        REQUIRE(first.run_id != second.run_id);
        REQUIRE(f.tasks->next_completion(bootstrap::task_owner(f.options)) == first.run_id);
        REQUIRE(f.command(tool::TeamStopRequest{}));
        CHECK_FALSE(f.tasks->next_completion(bootstrap::task_owner(f.options)));
        auto stale = co_await f.session->run_completion({first.run_id});
        REQUIRE(stale.has_value());
        CHECK_FALSE(stale->has_value());
        CHECK(f.provider.completions == 0);
        auto report = f.tasks->get(bootstrap::task_owner(f.options), first.run_id);
        REQUIRE(report.has_value());
        CHECK(report->result == first.report);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Reused team sessions trace the initiating turn of each assignment", "[bootstrap][teams][team-review]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor(), {.automatic_delivery = true, .enable_teams = true}, "{}", true};
        core::TurnId creation{}, assignment_id{}, correction{};
        creation.back() = std::byte{1};
        assignment_id.back() = std::byte{2};
        correction.back() = std::byte{3};
        REQUIRE(f.command(tool::TeamCreateRequest{"trace corrections", {{"member", "inspect", {}}}}, creation));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"task", "member", "inspect", {}}, assignment_id));
        const auto first = co_await f.settled("task");
        REQUIRE(first.state == bootstrap::TeamTaskState::in_review);
        REQUIRE(
            f.command(tool::TeamTaskUpdateRequest{"task", 1, tool::TeamTaskAction::revise, "check again"}, correction));
        const auto second = co_await f.settled("task");
        REQUIRE(second.state == bootstrap::TeamTaskState::in_review);
        REQUIRE(f.assembly->trace_repository() != nullptr);
        auto traces =
            co_await f.assembly->trace_repository()->list_turns({.agent_key = f.board().members.front().agent_key});
        REQUIRE(traces.has_value());
        REQUIRE(traces->size() == 2);
        CHECK(std::ranges::contains(*traces, assignment_id, &storage::TraceTurnRecord::parent_turn_id));
        CHECK(std::ranges::contains(*traces, correction, &storage::TraceTurnRecord::parent_turn_id));
        CHECK_FALSE(std::ranges::contains(*traces, creation, &storage::TraceTurnRecord::parent_turn_id));
        CHECK(traces->front().prompt_prefix_hash == traces->back().prompt_prefix_hash);
        co_await f.shutdown();
      },
      5s);
}
