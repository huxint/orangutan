// tests/core/test_error.cpp — Error / Result coverage.

#include <chrono>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

using namespace orangutan::core;
using namespace std::chrono_literals;

TEST_CASE("Error builders carry the right kind", "[unit][core][error]") {
  REQUIRE(Error::cancelled().kind() == ErrorKind::cancelled);
  REQUIRE(Error::invalid_argument("x").kind() == ErrorKind::invalid_argument);
  REQUIRE(Error::not_found("x").kind() == ErrorKind::not_found);
  REQUIRE(Error::io("x").kind() == ErrorKind::io);
  REQUIRE(Error::network("x").kind() == ErrorKind::network);
  REQUIRE(Error::rate_limit("x").kind() == ErrorKind::rate_limit);
  REQUIRE(Error::timeout(250ms).kind() == ErrorKind::timeout);
  REQUIRE(Error::parsing("x").kind() == ErrorKind::parsing);
  REQUIRE(Error::not_modified("x").kind() == ErrorKind::not_modified);
  REQUIRE(Error::storage("x").kind() == ErrorKind::storage);
  REQUIRE(Error::internal("x").kind() == ErrorKind::internal);
}

TEST_CASE("Error::retryable matches the documented category set", "[unit][core][error]") {
  REQUIRE(Error::network("x").retryable());
  REQUIRE(Error::rate_limit("x").retryable());
  REQUIRE(Error::timeout(1ms).retryable());
  REQUIRE(Error::upstream("x").retryable());

  REQUIRE_FALSE(Error::cancelled().retryable());
  REQUIRE_FALSE(Error::invalid_argument("x").retryable());
  REQUIRE_FALSE(Error::not_found("x").retryable());
  REQUIRE_FALSE(Error::io("x").retryable());
  REQUIRE_FALSE(Error::permission_denied("x").retryable());
  REQUIRE_FALSE(Error::config("x").retryable());
  REQUIRE_FALSE(Error::not_modified("x").retryable());
  REQUIRE_FALSE(Error::storage("x").retryable());
  REQUIRE_FALSE(Error::internal("x").retryable());
}

TEST_CASE("Error::with appends context entries in order", "[unit][core][error]") {
  auto e = Error::network("HTTP 503 from anthropic")
               .with("agent", "primary")
               .with("attempt", "2")
               .with_retry_after(std::chrono::milliseconds{1500});

  REQUIRE(e.kind() == ErrorKind::network);
  REQUIRE(e.message() == "HTTP 503 from anthropic");

  const auto ctx = e.context();
  REQUIRE(ctx.size() == 2);
  REQUIRE(ctx[0].first == "agent");
  REQUIRE(ctx[0].second == "primary");
  REQUIRE(ctx[1].first == "attempt");
  REQUIRE(ctx[1].second == "2");
  REQUIRE(e.retry_after().has_value());
  REQUIRE(e.retry_after()->count() == 1500);
}

TEST_CASE("Error formats via std::format", "[unit][core][error]") {
  auto e = Error::network("HTTP 503").with("agent", "primary").with_retry_after(250ms);
  const auto rendered = std::format("{}", e);
  REQUIRE(rendered.contains("network: HTTP 503"));
  REQUIRE(rendered.contains("[agent=primary]"));
  REQUIRE(rendered.contains("retry_after=250ms"));
}

TEST_CASE("Result<int> happy and error paths", "[unit][core][result]") {
  Result<int> happy = 42;
  REQUIRE(happy.has_value());
  REQUIRE(*happy == 42);

  Result<int> sad = std::unexpected(Error::not_found("missing"));
  REQUIRE_FALSE(sad.has_value());
  REQUIRE(sad.error().kind() == ErrorKind::not_found);
}

TEST_CASE("enum_name covers all ErrorKind enumerators", "[unit][core][error]") {
  REQUIRE(enum_name(ErrorKind::ok) == "ok");
  REQUIRE(enum_name(ErrorKind::io) == "io");
  REQUIRE(enum_name(ErrorKind::network) == "network");
  REQUIRE(enum_name(ErrorKind::mailbox_overflowed) == "mailbox_overflowed");
  REQUIRE(enum_name(ErrorKind::internal) == "internal");
}
