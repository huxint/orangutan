#include "evaluation.hpp"

#include <charconv>
#include <csignal>
#include <filesystem>
#include <print>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>

#include <oran/async/runtime.hpp>
#include <oran/bootstrap/provider_backend.hpp>
#include <oran/config/config.hpp>
#include <oran/core/error.hpp>
#include <oran/io/private_directory.hpp>

namespace {
using namespace orangutan;
void usage() {
  std::println("eval-context (--self-test | --config PATH) --output NEW_DIRECTORY\n"
               "  [--case all|goal_constraints|superseded_decision|completed_vs_pending|large_tool_result]\n"
               "  [--repeat 1..5] [--max-calls 1..512] [--context-tokens 8192..32768]\n"
               "  [--deadline-seconds 1..3600]\n"
               "Live runs read only credential variables named by the supplied config.\n"
               "Outputs are retained in a new private directory. Exit: 0 pass, 1 failed/incomplete, 2 invalid setup.");
}
}  // namespace

int main(int argc, char** argv) try {
  evaluation::Options options;
  std::string config_path;
  bool controlled = false;
  std::size_t deadline = 900;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    if (argument == "--help") {
      usage();
      return 0;
    }
    if (argument == "--self-test") {
      controlled = true;
      continue;
    }
    if (++i >= argc) {
      usage();
      return 2;
    }
    const std::string_view value{argv[i]};
    if (argument == "--config")
      config_path = value;
    else if (argument == "--output")
      options.output = value;
    else if (argument == "--case")
      options.selected_case = value;
    else {
      std::size_t number{};
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
      if (error != std::errc{} || end != value.data() + value.size()) {
        usage();
        return 2;
      }
      if (argument == "--repeat")
        options.repeats = number;
      else if (argument == "--max-calls")
        options.max_calls = number;
      else if (argument == "--context-tokens")
        options.context_tokens = number;
      else if (argument == "--deadline-seconds")
        deadline = number;
      else {
        usage();
        return 2;
      }
    }
  }
  if (controlled == !config_path.empty() || !evaluation::validate(options) || deadline < 1 || deadline > 3600) {
    usage();
    return 2;
  }
  async::Runtime runtime{{.io_workers = 1, .cpu_workers = 2}};
  auto coordinator = runtime.make_strand();
  std::optional<bootstrap::HttpProviderBackend> live;
  provider::Route route{.primary = {.profile = "controlled",
                                    .model = "controlled",
                                    .thinking_budget = std::nullopt,
                                    .cache = std::nullopt},
                        .fallbacks = {}};
  if (!controlled) {
    auto config = config::Config::load_file(config_path, {.strict_unknown_fields = true});
    if (!config) {
      std::println(stderr, "Unable to load evaluation provider config ({})", core::enum_name(config.error().kind()));
      return 2;
    }
    auto backend = bootstrap::HttpProviderBackend::build(
        *config,
        {.blocking_executor = runtime.cpu_executor(), .request_timeout = std::chrono::seconds{60}});
    if (!backend) {
      std::println(stderr, "Unable to construct evaluation provider ({})", core::enum_name(backend.error().kind()));
      return 2;
    }
    live.emplace(std::move(*backend));
    route = live->route();
  }
  std::error_code filesystem_error;
  if (!std::filesystem::create_directory(options.output, filesystem_error) || filesystem_error) {
    std::println(stderr, "Evaluation output must be a new directory under an existing parent");
    return 2;
  }
  options.output = std::filesystem::absolute(options.output).string();
  std::filesystem::permissions(options.output,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               filesystem_error);
  if (filesystem_error) {
    std::println(stderr, "Cannot protect evaluation directory");
    return 2;
  }
  auto directory = io::PrivateDirectory::open(options.output);
  if (!directory) {
    std::println(stderr, "Cannot open private evaluation directory");
    return 2;
  }
  auto lock = directory->lock("evaluation.lock");
  if (!lock) {
    std::println(stderr, "Cannot lock evaluation directory");
    return 2;
  }

  asio::cancellation_signal cancellation;
  asio::signal_set signals{coordinator, SIGINT, SIGTERM};
  signals.async_wait([&](const asio::error_code& error, int) {
    if (!error)
      cancellation.emit(asio::cancellation_type::all);
  });
  asio::steady_timer timer{coordinator, std::chrono::seconds{deadline}};
  timer.async_wait([&](const asio::error_code& error) {
    if (!error)
      cancellation.emit(asio::cancellation_type::all);
  });
  nlohmann::json report;
  bool finished = false;
  asio::co_spawn(
      coordinator,
      evaluation::run(options, live ? &live->system() : nullptr, route, coordinator, runtime.cpu_executor(), report),
      asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr exception, core::Result<void> result) {
        finished = true;
        if (exception || !result) {
          report["complete"] = false;
          report["passed"] = false;
          report["error_kind"] =
              exception ? "cancelled_or_internal" : std::string{core::enum_name(result.error().kind())};
        }
        timer.cancel();
        signals.cancel();
        runtime.stop();
      }));
  auto ran = runtime.run();
  if (!ran || !finished) {
    report["complete"] = false;
    report["passed"] = false;
  }
  evaluation::summarize(report);
  report["configured_model"] = route.primary.model;
  report["configured_protocol"] = core::enum_name(route.primary.protocol);
  report["deadline_seconds"] = deadline;
  auto saved = directory->write("report.json", report.dump(2) + "\n");
  if (!saved) {
    std::println(stderr, "Cannot write evaluation report");
    return 2;
  }
  std::println("Report: {}/report.json", options.output);
  return report.value("passed", false) ? 0 : 1;
} catch (const std::exception&) {
  std::println(stderr, "Evaluation setup failed");
  return 2;
}
