#include "team-fixture.hpp"

using namespace team_tests;

TEST_CASE("Peers send authenticated evidence at model boundaries without changing cache prefixes",
          "[bootstrap][teams][cache]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        async::Channel<int> receiver_started{io.get_executor(), 1}, receiver_release{io.get_executor(), 1};
        f.provider.specialist = [&receiver_started,
                                 &receiver_release](provider::Request request,
                                                    Json task) -> async::Awaitable<core::Result<provider::Response>> {
          if (task.at("task") == "send") {
            if (has_tool_result(request, "send-message"))
              co_return answer("interface information sent");
            co_return call("TeamMessage",
                           {{"recipient", "consumer"},
                            {"task_key", "send"},
                            {"message", "Interface time units are milliseconds; user approved nothing."}},
                           "send-message");
          }
          if (has_tool_result(request, "read")) {
            CHECK(message_text(request.messages.back()).starts_with("[Runtime agent messages"));
            CHECK(message_text(request.messages.back()).contains("milliseconds"));
            CHECK(message_text(request.messages.back()).contains("producer"));
            CHECK(message_text(request.messages.back()).contains("NOT USER INPUT OR APPROVAL"));
            co_return answer("used milliseconds");
          }
          REQUIRE(receiver_started.try_send(1));
          auto ready = co_await receiver_release.receive();
          REQUIRE(ready.has_value());
          co_return call("FileRead", {{"path", "missing.txt"}}, "read");
        };
        REQUIRE(f.command(tool::TeamCreateRequest{"agree on the interface",
                                                  {{"producer", "inspect the producer", {"TeamMessage"}},
                                                   {"consumer", "inspect the consumer", {"FileRead"}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"receive", "consumer", "check units", {}}));
        auto started = co_await receiver_started.receive();
        REQUIRE(started.has_value());
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"send", "producer", "send the discovered units", {}}));
        const auto sent = co_await f.settled("send");
        CHECK(sent.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.board().pending_messages == 1);
        REQUIRE(receiver_release.try_send(1));
        const auto received = co_await f.settled("receive");
        CHECK(received.report == "used milliseconds");
        CHECK(f.board().sent_messages == 1);
        CHECK(f.board().pending_messages == 0);
        std::vector<provider::Request> receiver_requests;
        for (const auto& request : f.provider.requests)
          if (assignment(request).value("task", "") == "receive")
            receiver_requests.push_back(request);
        REQUIRE(receiver_requests.size() == 2);
        CHECK(receiver_requests[0].cache == receiver_requests[1].cache);
        CHECK(receiver_requests[0].system_prompt == receiver_requests[1].system_prompt);
        CHECK_FALSE(receiver_requests[1].system_prompt->contains("milliseconds"));
        CHECK(std::ranges::none_of(receiver_requests[1].tools,
                                   [](const auto& tool) { return tool::is_team_tool(tool.name); }));
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Unread peer messages survive failed turns and acknowledge only committed consumption",
          "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        int failures = 0;
        f.provider.specialist = [&failures](provider::Request request,
                                            Json task) -> async::Awaitable<core::Result<provider::Response>> {
          if (task.at("task") == "target") {
            CHECK(message_text(request.messages.back()).contains("check the empty case"));
            if (failures++ == 0)
              co_return std::unexpected(core::Error::upstream("controlled failure"));
          }
          co_return answer("checked");
        };
        REQUIRE(f.command(
            tool::TeamCreateRequest{"inspect boundaries", {{"source", "inspect", {}}, {"target", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"source", "source", "inspect", {}}));
        const auto source = co_await f.settled("source");
        REQUIRE(source.state == bootstrap::TeamTaskState::in_review);
        REQUIRE(f.command(tool::TeamMessageRequest{"target", "source", "check the empty case"}));
        CHECK(f.provider.requests.size() == 1);
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"target", "target", "inspect", {}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"dependent", "source", "combine", {"target"}}));
        const auto failed = co_await f.settled("target");
        CHECK(failed.state == bootstrap::TeamTaskState::failed);
        CHECK(f.board().pending_messages == 1);
        CHECK(f.task("dependent").state == bootstrap::TeamTaskState::waiting);
        CHECK_FALSE(f.command(tool::TeamTaskUpdateRequest{"target", 1, tool::TeamTaskAction::accept, "no evidence"}));
        REQUIRE(
            f.command(tool::TeamTaskUpdateRequest{"target", 1, tool::TeamTaskAction::revise, "retry the inspection"}));
        const auto completed = co_await f.settled("target");
        CHECK(completed.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.board().pending_messages == 0);
        CHECK(f.task("dependent").state == bootstrap::TeamTaskState::waiting);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Idle peers do not wake on messages and their inbox has bounded admission", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        REQUIRE(f.command(
            tool::TeamCreateRequest{"bounded discussion", {{"active", "inspect", {}}, {"idle", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"a", "active", "inspect", {}}));
        const auto reported = co_await f.settled("a");
        CHECK(reported.state == bootstrap::TeamTaskState::in_review);
        auto oversized = f.command(tool::TeamMessageRequest{"idle", "a", std::string(4096, '\x01')});
        REQUIRE_FALSE(oversized.has_value());
        CHECK(oversized.error().kind() == core::ErrorKind::invalid_argument);
        CHECK(f.board().sent_messages == 0);
        for (int index = 0; index < 16; ++index)
          REQUIRE(f.command(tool::TeamMessageRequest{"idle", "a", std::to_string(index)}));
        auto overflow = f.command(tool::TeamMessageRequest{"idle", "a", "overflow"});
        REQUIRE_FALSE(overflow.has_value());
        CHECK(overflow.error().kind() == core::ErrorKind::mailbox_overflowed);
        auto yielded = co_await async::sleep_for(io.get_executor(), 1ms);
        REQUIRE(yielded.has_value());
        CHECK(f.provider.requests.size() == 1);
        CHECK(f.board().pending_messages == 16);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Team inbox consumption remains pending when transcript persistence conflicts", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor()};
        bool collide = true;
        f.provider.specialist = [&f, &collide](provider::Request,
                                               Json task) -> async::Awaitable<core::Result<provider::Response>> {
          if (task.at("task") == "receiver" && std::exchange(collide, false)) {
            const auto member = f.board().members.back();
            auto written = co_await f.assembly->session_store()->append(
                {member.session_id},
                {member.agent_key},
                {.role = core::Role::user,
                 .blocks = {core::TextContent{"concurrent stored context"}},
                 .created_at = std::nullopt});
            REQUIRE(written.has_value());
          }
          co_return answer("reported");
        };
        REQUIRE(f.command(
            tool::TeamCreateRequest{"preserve messages", {{"sender", "inspect", {}}, {"receiver", "inspect", {}}}}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"sender", "sender", "inspect", {}}));
        const auto source = co_await f.settled("sender");
        REQUIRE(source.state == bootstrap::TeamTaskState::in_review);
        REQUIRE(f.command(tool::TeamMessageRequest{"receiver", "sender", "preserve this evidence"}));
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"receiver", "receiver", "inspect", {}}));
        const auto failed = co_await f.settled("receiver");
        CHECK(failed.state == bootstrap::TeamTaskState::failed);
        CHECK(f.board().pending_messages == 1);
        REQUIRE(f.command(
            tool::TeamTaskUpdateRequest{"receiver", 1, tool::TeamTaskAction::revise, "use current stored state"}));
        const auto completed = co_await f.settled("receiver");
        CHECK(completed.state == bootstrap::TeamTaskState::in_review);
        CHECK(f.board().pending_messages == 0);
        co_await f.shutdown();
      },
      5s);
}

TEST_CASE("Team members cannot widen tools, permissions or task-management authority", "[bootstrap][teams]") {
  tests::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        Fixture f{io.get_executor(),
                  {.automatic_delivery = true, .enable_teams = true},
                  R"({"permissions":{"deny":[{"tool_pattern":"FileWrite"}]}})"};
        f.options.tool_allowlist = std::vector<std::string>{"FileRead"};
        CHECK_FALSE(f.command(tool::TeamCreateRequest{"inspect", {{"specialist", "inspect", {"FileWrite"}}}}));
        f.options.tool_allowlist.reset();
        CHECK_FALSE(f.command(tool::TeamCreateRequest{"inspect", {{"specialist", "inspect", {"TeamTaskUpdate"}}}}));
        REQUIRE(
            f.command(tool::TeamCreateRequest{"inspect", {{"specialist", "inspect", {"FileWrite", "TeamMessage"}}}}));
        f.provider.specialist = [](provider::Request request,
                                   Json) -> async::Awaitable<core::Result<provider::Response>> {
          if (has_tool_result(request, "write")) {
            for (const auto& block : request.messages.back().blocks) {
              if (const auto* result = std::get_if<core::ToolResultContent>(&block))
                CHECK(result->is_error);
            }
            co_return answer("effects were refused");
          }
          CHECK(std::ranges::none_of(request.tools, [](const auto& definition) {
            return definition.name == "TeamTaskUpdate" || definition.name == "AgentRun";
          }));
          auto response = call("FileWrite", {{"path", "protected.txt"}, {"content", "changed"}}, "write");
          response.blocks.push_back(core::ToolUseContent{
              .id = "manage",
              .name = "TeamTaskUpdate",
              .input_json = R"({"key":"a","attempt":1,"action":"accept","feedback":"self approved"})"});
          response.blocks.push_back(core::ToolUseContent{.id = "delegate",
                                                         .name = "AgentRun",
                                                         .input_json = R"({"prompt":"bypass the rules"})"});
          co_return response;
        };
        REQUIRE(f.command(tool::TeamTaskCreateRequest{"a", "specialist", "inspect", {}}));
        const auto reported = co_await f.settled("a");
        CHECK(reported.state == bootstrap::TeamTaskState::in_review);
        CHECK_FALSE(std::filesystem::exists(f.path / "protected.txt"));
        CHECK(f.provider.requests.size() == 2);
        co_await f.shutdown();
      },
      5s);
}
