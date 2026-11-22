#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::evaluation {

struct Scenario {
  std::string name;
  std::vector<core::Message> history{};
  nlohmann::json expected;
  std::string obsolete_decision{};
  bool large_tool{};
};

struct Options {
  std::string output;
  std::string selected_case{"all"};
  std::size_t repeats{1};
  std::size_t max_calls{192};
  std::size_t context_tokens{16384};
};

[[nodiscard]] std::vector<Scenario> scenarios();
[[nodiscard]] nlohmann::json grade(std::string_view answer, const Scenario& scenario);
void summarize(nlohmann::json& report);
[[nodiscard]] core::Result<void> validate(const Options& options);

/// A null provider selects a controlled plumbing check, never a model evaluation.
/// The host owns a new private output directory and waits for all borrowed work.
[[nodiscard]] async::Awaitable<core::Result<void>> run(const Options& options,
                                                       provider::System* backend,
                                                       provider::Route route,
                                                       asio::any_io_executor coordinator,
                                                       asio::any_io_executor worker,
                                                       nlohmann::json& report);

}  // namespace orangutan::evaluation
