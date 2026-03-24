#include "app/slash-commands.hpp"

#include "support/ut.hpp"

using namespace orangutan;
using namespace orangutan::app;

namespace {

boost::ut::suite slash_commands_suite = [] {
    using namespace boost::ut;

    "dispatches_shared_commands_from_declarative_table"_test = [] {
        bool status_called = false;

        const auto reply = dispatch_shared_slash_command("/status", {
                                                                        .status =
                                                                            [&] {
                                                                                status_called = true;
                                                                                return SlashCommandReply{.handled = true, .text = "status"};
                                                                            },
                                                                    });

        expect(status_called);
        expect(reply.handled);
        expect(reply.text == "status");
    };

    "no_arg_commands_do_not_consume_trailing_arguments"_test = [] {
        bool help_called = false;

        const auto reply = dispatch_shared_slash_command("/help extra", {
                                                                            .help =
                                                                                [&] {
                                                                                    help_called = true;
                                                                                    return SlashCommandReply{.handled = true, .text = "help"};
                                                                                },
                                                                        });

        expect(not help_called);
        expect(not reply.handled);
    };

    "resume_command_trims_whitespace_and_handles_missing_id"_test = [] {
        std::string captured_session_id;

        const auto resumed = dispatch_shared_slash_command("/resume    latest   ", {
                                                                                       .resume =
                                                                                           [&](const std::string &session_id) {
                                                                                               captured_session_id = session_id;
                                                                                               return SlashCommandReply{.handled = true, .text = "ok"};
                                                                                           },
                                                                                   });

        expect(resumed.handled);
        expect(resumed.text == "ok");
        expect(captured_session_id == "latest");

        const auto usage = dispatch_shared_slash_command("/resume", {});
        expect(usage.handled);
        expect(usage.text == "Usage: /resume <session-id>");
    };

    "registry_commands_dispatch_from_parsed_root_command"_test = [] {
        ToolRegistry registry;
        registry.register_tool({
            .definition = {.name = "task", .description = "Task tool"},
            .execute =
                [](const json &input) {
                    if (input.at("op") == "run") {
                        return std::string{"ran "} + input.at("id").get<std::string>();
                    }
                    return std::string{"listed"};
                },
        });

        const auto list_reply = handle_registry_slash_command("/tasks", &registry);
        expect(list_reply.handled);
        expect(list_reply.text.find("## Tasks") != std::string::npos);
        expect(list_reply.text.find("listed") != std::string::npos);

        const auto run_reply = handle_registry_slash_command("/tasks run   abc   ", &registry);
        expect(run_reply.handled);
        expect(run_reply.text.find("ran abc") != std::string::npos);
    };
};

} // namespace
