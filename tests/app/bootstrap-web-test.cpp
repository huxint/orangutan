#include <gtest/gtest.h>
#include <CLI/CLI.hpp>

struct WebCliOptions {
    bool web_mode = false;
    bool web_only = false;
    int web_port = 18080;
    std::string web_host = "127.0.0.1";
    std::string web_dir = "web/dist";
};

TEST(BootstrapWebTest, WebFlagsParseCorrectly) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--web-only", opts.web_only);
    app.add_option("--port", opts.web_port);
    app.add_option("--web-host", opts.web_host);
    app.add_option("--web-dir", opts.web_dir);

    const char *argv[] = {"test", "--web", "--port", "9090"};
    app.parse(4, argv);

    EXPECT_TRUE(opts.web_mode);
    EXPECT_FALSE(opts.web_only);
    EXPECT_EQ(opts.web_port, 9090);
}

TEST(BootstrapWebTest, WebOnlyFlagParsesCorrectly) {
    CLI::App app{"test"};
    WebCliOptions opts;
    app.add_flag("--web", opts.web_mode);
    app.add_flag("--web-only", opts.web_only);
    app.add_option("--port", opts.web_port);

    const char *argv[] = {"test", "--web-only", "--port", "8000"};
    app.parse(4, argv);

    EXPECT_FALSE(opts.web_mode);
    EXPECT_TRUE(opts.web_only);
    EXPECT_EQ(opts.web_port, 8000);
}
