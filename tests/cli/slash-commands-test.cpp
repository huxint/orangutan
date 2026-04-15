#include "cli/slash-commands.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;
using namespace orangutan::cli;

namespace {

    TEST_CASE("dispatches_shared_commands_from_declarative_table") {
        bool status_called = false;

        const auto reply = dispatch_shared_slash_command("/status", {
                                                                        .status =
                                                                            [&] {
                                                                                status_called = true;
                                                                                return SlashCommandReply{.handled = true, .text = "status"};
                                                                            },
                                                                    });

        CHECK(status_called);
        CHECK(reply.handled);
        CHECK(reply.text == "status");
    };

    TEST_CASE("no_arg_commands_do_not_consume_trailing_arguments") {
        bool help_called = false;

        const auto reply = dispatch_shared_slash_command("/help extra", {
                                                                            .help =
                                                                                [&] {
                                                                                    help_called = true;
                                                                                    return SlashCommandReply{.handled = true, .text = "help"};
                                                                                },
                                                                        });

        CHECK_FALSE(help_called);
        CHECK_FALSE(reply.handled);
    };

    TEST_CASE("resume_command_trims_whitespace_and_handles_missing_id") {
        std::string captured_session_id;

        const auto resumed = dispatch_shared_slash_command("/resume    latest   ", {
                                                                                       .resume =
                                                                                           [&](const std::string &session_id) {
                                                                                               captured_session_id = session_id;
                                                                                               return SlashCommandReply{.handled = true, .text = "ok"};
                                                                                           },
                                                                                   });

        CHECK(resumed.handled);
        CHECK(resumed.text == "ok");
        CHECK(captured_session_id == "latest");

        const auto usage = dispatch_shared_slash_command("/resume", {});
        CHECK(usage.handled);
        CHECK(usage.text == "Usage: /resume <session-id>");
    };

    TEST_CASE("registry_commands_dispatch_from_parsed_root_command") {
        ToolRegistry registry;
        registry.register_tool({
            .definition = {.name = "automation", .description = "Automation tool"},
            .execute =
                [](const nlohmann::json &input) {
                    if (input.at("op") == "run") {
                        return std::string{"Ran automation ("} + input.at("id").get<std::string>() + ").";
                    }
                    return std::string{R"([{"id":"abc","name":"repo-check","enabled":true,"paused":false,"trigger":{"type":"cron"}}])"};
                },
        });

        const auto list_reply = handle_registry_slash_command("/automation", &registry);
        CHECK(list_reply.handled);
        CHECK(list_reply.text.contains("## Automation"));
        CHECK(list_reply.text.contains("repo-check"));

        const auto run_reply = handle_registry_slash_command("/automation run   abc   ", &registry);
        CHECK(run_reply.handled);
        CHECK(run_reply.text.contains("Ran automation (abc)."));
    };

} // namespace
