#include <array>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

TEST_CASE("dispatch refuses an effect when one required capability is ungranted", "[integration][tool][policy]") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    const std::array rules{permission::Rule{.verdict = permission::Verdict::allow,
                                            .tool_pattern = "*",
                                            .capability = core::Capability::read_file}};
    permission::RecordingAuditSink audit;
    auto context = tool::DispatchContext::for_now(io.get_executor(), rules, audit);
    context.mode = permission::Mode::strict;
    bool executed = false;
    tool::Registry registry;
    auto definition = core::ToolDef::with_no_input("CopyNote", "Read and write a note");
    definition.required_capabilities = {core::Capability::read_file, core::Capability::write_file};
    REQUIRE(registry.add(
        std::move(definition),
        [&executed](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
          executed = true;
          co_return tool::Output::text_only("copied");
        }));

    const auto result = co_await registry.dispatch("CopyNote", "{}", context);

    CHECK_FALSE(executed);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind() == core::ErrorKind::permission_denied);
    REQUIRE(audit.events().size() == 1);
    CHECK(audit.events().front().outcome == permission::AuditOutcome::deny);
  });
}
