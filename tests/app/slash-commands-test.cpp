#include "app/slash-commands.hpp"

#include <gtest/gtest.h>

using namespace orangutan;
using namespace orangutan::app;

TEST(SlashCommandsTest, DispatchesSharedCommandsFromDeclarativeTable) {
    bool status_called = false;

    const auto reply = dispatch_shared_slash_command("/status", {
                                                                    .status =
                                                                        [&] {
                                                                            status_called = true;
                                                                            return SlashCommandReply{.handled = true, .text = "status"};
                                                                        },
                                                                });

    EXPECT_TRUE(status_called);
    EXPECT_TRUE(reply.handled);
    EXPECT_EQ(reply.text, "status");
}

TEST(SlashCommandsTest, NoArgCommandsDoNotConsumeTrailingArguments) {
    bool help_called = false;

    const auto reply = dispatch_shared_slash_command("/help extra", {
                                                                        .help =
                                                                            [&] {
                                                                                help_called = true;
                                                                                return SlashCommandReply{.handled = true, .text = "help"};
                                                                            },
                                                                    });

    EXPECT_FALSE(help_called);
    EXPECT_FALSE(reply.handled);
}

TEST(SlashCommandsTest, ResumeCommandTrimsWhitespaceAndHandlesMissingId) {
    std::string captured_session_id;

    const auto resumed = dispatch_shared_slash_command("/resume    latest   ", {
                                                                                   .resume =
                                                                                       [&](const std::string &session_id) {
                                                                                           captured_session_id = session_id;
                                                                                           return SlashCommandReply{.handled = true, .text = "ok"};
                                                                                       },
                                                                               });

    EXPECT_TRUE(resumed.handled);
    EXPECT_EQ(resumed.text, "ok");
    EXPECT_EQ(captured_session_id, "latest");

    const auto usage = dispatch_shared_slash_command("/resume", {});
    EXPECT_TRUE(usage.handled);
    EXPECT_EQ(usage.text, "Usage: /resume <session-id>");
}

TEST(SlashCommandsTest, RegistryCommandsDispatchFromParsedRootCommand) {
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
    EXPECT_TRUE(list_reply.handled);
    EXPECT_NE(list_reply.text.find("## Tasks"), std::string::npos);
    EXPECT_NE(list_reply.text.find("listed"), std::string::npos);

    const auto run_reply = handle_registry_slash_command("/tasks run   abc   ", &registry);
    EXPECT_TRUE(run_reply.handled);
    EXPECT_NE(run_reply.text.find("ran abc"), std::string::npos);
}
