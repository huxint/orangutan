#include <gtest/gtest.h>
#include <CLI/CLI.hpp>

struct WebCliOptions {
    bool cli_mode = false;
    bool web_mode = false;
    bool channel_mode = false;
    int web_port = 18080;
    std::string web_host = "127.0.0.1";
    std::string web_dir = "web/dist";
};

TEST(BootstrapWebTest, WebFlagsParseCorrectly) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--cli", opts.cli_mode);
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--channel", opts.channel_mode);
    app.add_option("--port", opts.web_port);
    app.add_option("--web-host", opts.web_host);
    app.add_option("--web-dir", opts.web_dir);

    const char *argv[] = {"test", "--web", "--port", "9090"};
    app.parse(4, argv);

    EXPECT_TRUE(opts.web_mode);
    EXPECT_FALSE(opts.cli_mode);
    EXPECT_FALSE(opts.channel_mode);
    EXPECT_EQ(opts.web_port, 9090);
}

TEST(BootstrapWebTest, CombinedFlagsParseCorrectly) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--cli", opts.cli_mode);
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--channel", opts.channel_mode);
    app.add_option("--port", opts.web_port);

    const char *argv[] = {"test", "--cli", "--web", "--channel", "--port", "8000"};
    app.parse(6, argv);

    EXPECT_TRUE(opts.cli_mode);
    EXPECT_TRUE(opts.web_mode);
    EXPECT_TRUE(opts.channel_mode);
    EXPECT_EQ(opts.web_port, 8000);
}

TEST(BootstrapWebTest, MissingEntryFlagsLeavesAllModesDisabled) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--cli", opts.cli_mode);
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--channel", opts.channel_mode);

    const char *argv[] = {"test"};
    app.parse(1, argv);

    EXPECT_FALSE(opts.cli_mode);
    EXPECT_FALSE(opts.web_mode);
    EXPECT_FALSE(opts.channel_mode);
}

TEST(BootstrapWebTest, LegacyServeFlagIsRejected) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--cli", opts.cli_mode);
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--channel", opts.channel_mode);

    const char *argv[] = {"test", "--serve"};
    EXPECT_THROW(app.parse(2, argv), CLI::ParseError);
}
