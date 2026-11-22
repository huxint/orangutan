// src/oran-core/message.cpp — `core::Message` convenience factories.

#include <oran/core/message.hpp>

#include <string>
#include <utility>

#include <oran/core/content.hpp>
#include <oran/core/role.hpp>

namespace orangutan::core {

Message Message::user_text(std::string text) {
  return Message{
      .role = Role::user,
      .blocks = {TextContent{.text = std::move(text)}},
      .created_at = std::nullopt,
  };
}

Message Message::assistant_text(std::string text) {
  return Message{
      .role = Role::assistant,
      .blocks = {TextContent{.text = std::move(text)}},
      .created_at = std::nullopt,
  };
}

}  // namespace orangutan::core
