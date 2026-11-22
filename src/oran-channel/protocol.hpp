#pragma once
#include <nlohmann/json.hpp>
#include <oran/channel/adapter.hpp>
#include <oran/core/error.hpp>
namespace orangutan::channel::detail {
using Json = nlohmann::json;
using core::Error;
using core::Result;
[[nodiscard]] Result<Json> parse(std::string_view text);
[[nodiscard]] std::string id(const Json& value);
[[nodiscard]] Result<std::optional<Message>> checked(Message message);
[[nodiscard]] std::string segment(std::string_view text);
[[nodiscard]] Result<void> validate(const Message& message, Platform platform);
[[nodiscard]] Result<Receipt> accept(Platform platform, const Response& response);
}  // namespace orangutan::channel::detail
