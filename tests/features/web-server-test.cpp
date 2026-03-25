#include "features/web/web-server.hpp"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

TEST_CASE("starts_and_stops_cleanly") {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    CHECK(server.port() > 0);
    CHECK(server.is_running());
    server.stop();
    CHECK_FALSE(server.is_running());
};

TEST_CASE("serves_health_endpoint") {
    orangutan::WebServer server;
    server.start("127.0.0.1", 0);
    httplib::Client cli("127.0.0.1", server.port());
    const auto res = cli.Get("/api/health");
    INFO("expected /api/health response");
    REQUIRE(static_cast<bool>(res));
    CHECK(res->status == 200);
    server.stop();
};

} // namespace
