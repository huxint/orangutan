#include "features/web/web-server.hpp"
#include "support/ut.hpp"

#include <chrono>
#include <thread>

namespace {

boost::ut::suite web_server_suite = [] {
    using namespace boost::ut;

    "starts_and_stops_cleanly"_test = [] {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        expect(server.port() > 0_i);
        expect(server.is_running());
        server.stop();
        expect(not server.is_running());
    };

    "serves_health_endpoint"_test = [] {
        orangutan::WebServer server;
        server.start("127.0.0.1", 0);
        httplib::Client cli("127.0.0.1", server.port());
        const auto res = cli.Get("/api/health");
        expect(static_cast<bool>(res) >> fatal) << "expected /api/health response";
        expect(res->status == 200_i);
        server.stop();
    };
};

} // namespace
