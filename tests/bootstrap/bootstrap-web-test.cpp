#include "bootstrap/cli-options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <CLI/CLI.hpp>

#include <array>

namespace {

    void configure_app(CLI::App &app, orangutan::bootstrap::CliOptions &opts) {
        CLI::Option *resume_flag = nullptr;
        CLI::Option *protect_flag = nullptr;
        orangutan::bootstrap::configure_cli_app(app, opts, resume_flag, protect_flag);
    }

    TEST_CASE("web_flags_parse_correctly") {
        orangutan::bootstrap::CliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        std::array<const char *, 4> argv = {"test", "--web", "--port", "9090"};
        app.parse(4, argv.data());

        CHECK(opts.web_mode);
        CHECK_FALSE(opts.cli_mode);
        CHECK_FALSE(opts.channel_mode);
        CHECK(opts.web_port == 9090);
    };

    TEST_CASE("combined_flags_parse_correctly") {
        orangutan::bootstrap::CliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        std::array<const char *, 6> argv = {"test", "--cli", "--web", "--channel", "--port", "8000"};
        app.parse(6, argv.data());

        CHECK(opts.cli_mode);
        CHECK(opts.web_mode);
        CHECK(opts.channel_mode);
        CHECK(opts.web_port == 8000);
    };

    TEST_CASE("missing_entry_flags_leaves_all_modes_disabled") {
        orangutan::bootstrap::CliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        std::array<const char *, 1> argv = {"test"};
        app.parse(1, argv.data());

        CHECK_FALSE(opts.cli_mode);
        CHECK_FALSE(opts.web_mode);
        CHECK_FALSE(opts.channel_mode);
    };

    TEST_CASE("legacy_serve_flag_is_rejected") {
        orangutan::bootstrap::CliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        std::array<const char *, 2> argv = {"test", "--serve"};
        REQUIRE_THROWS_AS(app.parse(2, argv.data()), CLI::ParseError);
    };

} // namespace
