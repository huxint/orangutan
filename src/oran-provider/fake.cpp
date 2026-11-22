// src/oran-provider/fake.cpp — scripted provider for spec 0017.

#include <oran/provider/fake.hpp>

#include <concepts>
#include <cstddef>
#include <expected>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/this_coro.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/content.hpp>

namespace orangutan::provider {
namespace {

[[nodiscard]] core::Error empty_turn_error(std::size_t cursor) {
  return core::Error::internal("provider fake: scripted turn has no body").with("turn", std::to_string(cursor));
}

[[nodiscard]] core::Error exhausted_error(std::size_t cursor) {
  return core::Error::internal("provider fake: plan exhausted").with("turn", std::to_string(cursor));
}

/// Append `text` to the trailing `TextContent` block, opening a new one if
/// the stream ends in a different block kind (or has no blocks yet).
void accumulate_text(Response& response, std::string_view text) {
  if (response.blocks.empty() || !std::holds_alternative<core::TextContent>(response.blocks.back())) {
    response.blocks.emplace_back(core::TextContent{});
  }
  std::get<core::TextContent>(response.blocks.back()).text.append(text);
}

void accumulate_thinking(Response& response, std::string_view text) {
  if (response.blocks.empty() || !std::holds_alternative<core::ThinkingContent>(response.blocks.back())) {
    response.blocks.emplace_back(core::ThinkingContent{});
  }
  std::get<core::ThinkingContent>(response.blocks.back()).thinking.append(text);
}

/// Append `delta` to the trailing `ToolUseContent` whose id matches. Returns
/// `false` if no matching open block exists.
[[nodiscard]] bool accumulate_tool_input(Response& response, std::string_view id, std::string_view delta) {
  for (auto it = response.blocks.rbegin(); it != response.blocks.rend(); ++it) {
    if (auto* tool = std::get_if<core::ToolUseContent>(&*it); tool != nullptr && tool->id == id) {
      tool->input_json.append(delta);
      return true;
    }
  }
  return false;
}

}  // namespace

class FakeProvider::Impl {
public:
  explicit Impl(std::vector<ScriptedTurn> plan) : plan_{std::move(plan)} {}

  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send(Request request, ModelTarget target, EventSink* sink) const {
    static_cast<void>(request);
    static_cast<void>(target);

    const std::size_t cursor = reserve_slot();

    if (cursor >= plan_.size()) {
      co_return std::unexpected(exhausted_error(cursor));
    }

    const auto& turn = plan_[cursor];

    if (turn.latency.count() > 0) {
      auto executor = co_await asio::this_coro::executor;
      auto slept = co_await async::sleep_for(executor, turn.latency);
      if (!slept) {
        co_return std::unexpected(std::move(slept).error());
      }
    }

    if (turn.error.has_value()) {
      co_return std::unexpected(*turn.error);
    }

    if (turn.response.has_value()) {
      Response response = *turn.response;
      if (sink != nullptr) {
        sink->on_done(response.stop_reason);
      }
      co_return response;
    }

    if (turn.deltas.empty()) {
      co_return std::unexpected(empty_turn_error(cursor));
    }

    Response response;
    bool ended = false;
    for (const auto& delta : turn.deltas) {
      std::visit(
          [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::same_as<T, TextDelta>) {
              if (sink != nullptr) {
                sink->on_text_delta(concrete.text);
              }
              accumulate_text(response, concrete.text);
            } else if constexpr (std::same_as<T, ThinkingDelta>) {
              if (sink != nullptr) {
                sink->on_thinking_delta(concrete.text);
              }
              accumulate_thinking(response, concrete.text);
            } else if constexpr (std::same_as<T, ToolStart>) {
              if (sink != nullptr) {
                sink->on_tool_start(concrete.id, concrete.name);
              }
              response.blocks.emplace_back(core::ToolUseContent{
                  .id = concrete.id,
                  .name = concrete.name,
                  .input_json = {},
              });
            } else if constexpr (std::same_as<T, ToolInputDelta>) {
              if (sink != nullptr) {
                sink->on_tool_delta(concrete.id, concrete.input_delta);
              }
              static_cast<void>(accumulate_tool_input(response, concrete.id, concrete.input_delta));
            } else if constexpr (std::same_as<T, StreamEnd>) {
              response.stop_reason = concrete.stop_reason;
              if (concrete.usage.has_value()) {
                response.usage = *concrete.usage;
              }
              if (concrete.model_used.has_value()) {
                response.model_used = *concrete.model_used;
              }
              if (sink != nullptr) {
                sink->on_done(concrete.stop_reason);
              }
              ended = true;
            }
          },
          delta);
      if (ended) {
        break;
      }
    }

    if (!ended && sink != nullptr) {
      sink->on_done(response.stop_reason);
    }

    co_return response;
  }

  [[nodiscard]] std::size_t turns_consumed() const noexcept {
    const std::lock_guard lock{mu_};
    return cursor_;
  }

  [[nodiscard]] std::size_t plan_size() const noexcept {
    return plan_.size();
  }

  [[nodiscard]] bool exhausted() const noexcept {
    return turns_consumed() >= plan_size();
  }

private:
  [[nodiscard]] std::size_t reserve_slot() const {
    const std::lock_guard lock{mu_};
    return cursor_++;
  }

  std::vector<ScriptedTurn> plan_;
  mutable std::mutex mu_;
  mutable std::size_t cursor_{0};
};

FakeProvider::FakeProvider(std::vector<ScriptedTurn> plan) : impl_{std::make_unique<Impl>(std::move(plan))} {}

FakeProvider::~FakeProvider() = default;

async::Awaitable<core::Result<Response>>
FakeProvider::send(Request request, ModelTarget target, EventSink* sink) const {
  return impl_->send(std::move(request), std::move(target), sink);
}

std::size_t FakeProvider::turns_consumed() const noexcept {
  return impl_->turns_consumed();
}

std::size_t FakeProvider::plan_size() const noexcept {
  return impl_->plan_size();
}

bool FakeProvider::exhausted() const noexcept {
  return impl_->exhausted();
}

}  // namespace orangutan::provider
