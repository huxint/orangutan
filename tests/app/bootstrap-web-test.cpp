#include <catch2/catch_test_macros.hpp>
#include <CLI/CLI.hpp>

#include <string>

struct WebCliOptions {
    bool cli_mode = false;
    bool web_mode = false;
    bool channel_mode = false;
    int web_port = 18080;
    std::string web_host = "127.0.0.1";
    std::string web_dir = "web/dist";
};

namespace {

    void configure_app(CLI::App &app, WebCliOptions &opts) {
        app.add_flag("--cli", opts.cli_mode);
        app.add_flag("--web", opts.web_mode);
        app.add_flag("--channel", opts.channel_mode);
        app.add_option("--port", opts.web_port);
        app.add_option("--web-host", opts.web_host);
        app.add_option("--web-dir", opts.web_dir);
    }

    TEST_CASE("web_flags_parse_correctly") {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--web", "--port", "9090"};
        app.parse(4, argv);

        CHECK(opts.web_mode);
        CHECK_FALSE(opts.cli_mode);
        CHECK_FALSE(opts.channel_mode);
        CHECK(opts.web_port == 9090);
    };

    TEST_CASE("combined_flags_parse_correctly") {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--cli", "--web", "--channel", "--port", "8000"};
        app.parse(6, argv);

        CHECK(opts.cli_mode);
        CHECK(opts.web_mode);
        CHECK(opts.channel_mode);
        CHECK(opts.web_port == 8000);
    };

    TEST_CASE("missing_entry_flags_leaves_all_modes_disabled") {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test"};
        app.parse(1, argv);

        CHECK_FALSE(opts.cli_mode);
        CHECK_FALSE(opts.web_mode);
        CHECK_FALSE(opts.channel_mode);
    };

    TEST_CASE("legacy_serve_flag_is_rejected") {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--serve"};
        REQUIRE_THROWS_AS(app.parse(2, argv), CLI::ParseError);
    };

} // namespace
