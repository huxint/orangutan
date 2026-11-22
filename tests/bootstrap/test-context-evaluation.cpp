#include "../../eval/context/evaluation.hpp"

#include <filesystem>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/io/private_directory.hpp>

#include "../test-helpers/run_async.hpp"

namespace evaluation = orangutan::evaluation;
namespace core = orangutan::core;
namespace async = orangutan::async;
namespace provider = orangutan::provider;
namespace test = orangutan::tests;
using nlohmann::json;

namespace {
struct TempEvaluation {
  std::filesystem::path path;
  TempEvaluation() {
    auto id = core::generate_turn_id();
    REQUIRE(id);
    path = std::filesystem::temp_directory_path() / ("oran-eval-" + core::format_turn_id_hex(*id));
    auto directory = orangutan::io::PrivateDirectory::open(path.string());
    REQUIRE(directory);
  }
  ~TempEvaluation() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
provider::Route route() {
  return {.primary = {.profile = "controlled",
                      .model = "controlled",
                      .thinking_budget = std::nullopt,
                      .cache = std::nullopt},
          .fallbacks = {}};
}
}  // namespace

TEST_CASE("Context evaluation grading rejects lost constraints and stale or invented completion",
          "[bootstrap][evaluation]") {
  const auto scenarios = evaluation::scenarios();
  const auto& scenario = scenarios[1];
  auto correct = evaluation::grade(scenario.expected.dump(), scenario);
  REQUIRE(correct["passed"] == true);
  auto reordered = scenario.expected;
  reordered["constraints"] = {"preserve_user_records", "no_new_dependencies"};
  REQUIRE(evaluation::grade(reordered.dump(), scenario)["passed"] == true);
  auto wrong = scenario.expected;
  wrong["constraints"] = {"no_new_dependencies"};
  wrong["decision"] = "rewrite_parser";
  wrong["completed"].push_back("regression_tests");
  const auto graded = evaluation::grade(wrong.dump(), scenario);
  REQUIRE(graded["passed"] == false);
  REQUIRE(graded["lost_constraints"] == true);
  REQUIRE(graded["stale_decision"] == true);
  REQUIRE(graded["false_completion"] == true);
  REQUIRE(evaluation::grade("not JSON", scenario)["valid_json"] == false);
  REQUIRE(evaluation::grade("[]", scenario)["passed"] == false);
}

TEST_CASE("Controlled evaluation exercises compaction and database reopening", "[bootstrap][evaluation]") {
  TempEvaluation temp;
  test::run_async(
      [&](asio::io_context& io) -> async::Awaitable<void> {
        evaluation::Options options{.output = temp.path.string(), .selected_case = "goal_constraints"};
        json report;
        auto result = co_await evaluation::run(options, nullptr, route(), io.get_executor(), io.get_executor(), report);
        REQUIRE(result);
        REQUIRE(report["mode"] == "controlled_plumbing_check");
        REQUIRE(report["complete"] == true);
        REQUIRE(report["passed"] == true);
        REQUIRE(report["cases"].size() == 2);
        const auto& phases = report["cases"][1]["phases"];
        REQUIRE(phases[0]["checkpoint_after"].get<int>() >= 2);
        REQUIRE(phases[1]["checkpoint_after"] > phases[1]["checkpoint_before"]);
        REQUIRE(phases[1]["stored_messages"] > phases[0]["stored_messages"]);
        evaluation::summarize(report);
        REQUIRE(report["totals"]["phases"] == 4);
        REQUIRE(report["totals"]["paired_regressions"] == 0);
        REQUIRE(report["variants"]["compacted"]["pass_rate"] == 1.0);
      },
      std::chrono::seconds{10});
}

TEST_CASE("Evaluation call budget refuses further provider attempts", "[bootstrap][evaluation]") {
  TempEvaluation temp;
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    evaluation::Options options{.output = temp.path.string(), .selected_case = "goal_constraints", .max_calls = 1};
    json report;
    auto result = co_await evaluation::run(options, nullptr, route(), io.get_executor(), io.get_executor(), report);
    REQUIRE_FALSE(result);
    REQUIRE(report["complete"] == false);
    REQUIRE(report["attempts"] == 1);
    REQUIRE(report["passed"] == false);
  });
}

TEST_CASE("Provider account errors are operational failures and stop evaluation", "[bootstrap][evaluation]") {
  TempEvaluation temp;
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    class EmptyAccount final : public provider::System {
    public:
      async::Awaitable<core::Result<provider::Response>>
      send(provider::Request, provider::ModelTarget, provider::EventSink*) const override {
        co_return std::unexpected(
            core::Error::invalid_argument("provider rejected request").with("http_status", "402"));
      }
    } backend;
    evaluation::Options options{.output = temp.path.string()};
    json report;
    auto result = co_await evaluation::run(options, &backend, route(), io.get_executor(), io.get_executor(), report);
    REQUIRE_FALSE(result);
    REQUIRE(report["attempts"] == 1);
    REQUIRE(report["complete"] == false);
    REQUIRE(report["cases"][0]["phases"][0]["http_status"] == 402);
    evaluation::summarize(report);
    REQUIRE(report["totals"]["operational_failures"] == 1);
    REQUIRE(report["totals"]["invalid_answers"] == 0);
    REQUIRE(report["totals"]["graded_phases"] == 0);
  });
}

TEST_CASE("Cancelling evaluation joins the provider and preserves an incomplete report", "[bootstrap][evaluation]") {
  TempEvaluation temp;
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    asio::cancellation_signal cancellation;
    class WaitingProvider final : public provider::System {
    public:
      WaitingProvider(asio::any_io_executor executor, asio::cancellation_signal& cancellation)
          : executor_{executor}, cancellation_{cancellation} {}
      async::Awaitable<core::Result<provider::Response>>
      send(provider::Request, provider::ModelTarget, provider::EventSink*) const override {
        asio::post(executor_, [this] { cancellation_.emit(asio::cancellation_type::all); });
        asio::steady_timer wait{executor_, std::chrono::seconds{5}};
        co_await wait.async_wait(asio::use_awaitable);
        co_return std::unexpected(core::Error::internal("cancellation did not arrive"));
      }

    private:
      asio::any_io_executor executor_;
      asio::cancellation_signal& cancellation_;
    } backend{io.get_executor(), cancellation};
    evaluation::Options options{.output = temp.path.string(), .selected_case = "goal_constraints"};
    json report;
    auto result = co_await asio::co_spawn(
        io,
        evaluation::run(options, &backend, route(), io.get_executor(), io.get_executor(), report),
        asio::bind_cancellation_slot(cancellation.slot(), asio::use_awaitable));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind() == core::ErrorKind::cancelled);
    REQUIRE(report["complete"] == false);
    REQUIRE(report["attempts"] == 1);
    REQUIRE(report["cases"][0]["phases"][0]["error_kind"] == "cancelled");
  });
}
