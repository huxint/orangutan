// tests/core/test_message.cpp — `core::Message` helpers and equality.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>
#include <oran/core/time.hpp>

using orangutan::core::Content;
using orangutan::core::holds_text;
using orangutan::core::Message;
using orangutan::core::Role;
using orangutan::core::text_view;
using orangutan::core::TextContent;
using orangutan::core::Time;
using orangutan::core::ToolUseContent;

TEST_CASE("Message::user_text builds a single-block user message", "[unit][core][message]") {
  const auto m = Message::user_text("hello world");

  REQUIRE(m.role == Role::user);
  REQUIRE(m.blocks.size() == 1);
  REQUIRE(holds_text(m.blocks.front()));
  REQUIRE(*text_view(m.blocks.front()) == "hello world");
  REQUIRE_FALSE(m.created_at.has_value());
}

TEST_CASE("Message::assistant_text builds a single-block assistant message", "[unit][core][message]") {
  const auto m = Message::assistant_text("hi back");

  REQUIRE(m.role == Role::assistant);
  REQUIRE(m.blocks.size() == 1);
  REQUIRE(holds_text(m.blocks.front()));
  REQUIRE(*text_view(m.blocks.front()) == "hi back");
}

TEST_CASE("Message equality is member-wise across role, blocks, and timestamp", "[unit][core][message]") {
  const Message a = Message::user_text("x");
  const Message b = Message::user_text("x");
  Message c = Message::user_text("x");
  c.role = Role::assistant;
  Message d = Message::user_text("x");
  d.created_at = Time::epoch();
  Message e = Message::user_text("x");
  e.created_at = Time::epoch();

  REQUIRE(a == b);
  REQUIRE_FALSE(a == c);
  REQUIRE_FALSE(a == d);
  REQUIRE(d == e);
}

TEST_CASE("Message can carry mixed-alternative blocks", "[unit][core][message]") {
  Message m;
  m.role = Role::assistant;
  m.blocks.push_back(TextContent{.text = "Let me look that up."});
  m.blocks.push_back(ToolUseContent{
      .id = "tool-1",
      .name = "FileRead",
      .input_json = R"({"path":"README.md"})",
  });

  REQUIRE(m.blocks.size() == 2);
  REQUIRE(holds_text(m.blocks[0]));
  REQUIRE_FALSE(holds_text(m.blocks[1]));
  REQUIRE(*text_view(m.blocks[0]) == "Let me look that up.");
}
