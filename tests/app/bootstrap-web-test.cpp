#include "support/ut.hpp"
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

boost::ut::suite bootstrap_web_suite = [] {
    using namespace boost::ut;

    "web_flags_parse_correctly"_test = [] {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--web", "--port", "9090"};
        app.parse(4, argv);

        expect(opts.web_mode);
        expect(not opts.cli_mode);
        expect(not opts.channel_mode);
        expect(opts.web_port == 9090_i);
    };

    "combined_flags_parse_correctly"_test = [] {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--cli", "--web", "--channel", "--port", "8000"};
        app.parse(6, argv);

        expect(opts.cli_mode);
        expect(opts.web_mode);
        expect(opts.channel_mode);
        expect(opts.web_port == 8000_i);
    };

    "missing_entry_flags_leaves_all_modes_disabled"_test = [] {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test"};
        app.parse(1, argv);

        expect(not opts.cli_mode);
        expect(not opts.web_mode);
        expect(not opts.channel_mode);
    };

    "legacy_serve_flag_is_rejected"_test = [] {
        WebCliOptions opts;
        CLI::App app{"test"};
        configure_app(app, opts);

        const char *argv[] = {"test", "--serve"};
        expect(throws<CLI::ParseError>([&] {
            app.parse(2, argv);
        }));
    };
};

} // namespace
