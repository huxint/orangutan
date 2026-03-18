#include "features/cron/parser.hpp"
#include "features/cron/store.hpp"
#include "features/heartbeat/scheduler.hpp"
#include "features/tools/builtin/cron.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

class CronToolTest : public ::testing::Test {
protected:
    std::string store_path;
    std::unique_ptr<CronStore> store;
    std::unique_ptr<HeartbeatScheduler> scheduler;
    ToolRuntimeContext ctx{};
    ToolRegistry registry;
    int fire_count = 0;
    std::string last_fired_name;

    void SetUp() override {
        store_path = std::string(std::getenv("HOME")) + "/.orangutan/test-cron-tool/jobs.json";
        std::filesystem::remove_all(std::filesystem::path(store_path).parent_path());

        store = std::make_unique<CronStore>(store_path);
        scheduler = std::make_unique<HeartbeatScheduler>([this](const HeartbeatJob &job) {
            ++fire_count;
            last_fired_name = job.name;
        });

        ctx.cron_store = store.get();
        ctx.heartbeat_scheduler = scheduler.get();
        register_cron_tool(registry, &ctx);
    }

    void TearDown() override {
        std::filesystem::remove_all(std::filesystem::path(store_path).parent_path());
    }

    std::string exec(const json &input) {
        ToolUseBlock call;
        call.name = "cron";
        call.input = input;
        auto result = registry.execute(call);
        return result.content;
    }
};

} // namespace

TEST_F(CronToolTest, AddCreatesJob) {
    auto result = exec({{"op", "add"}, {"name", "test"}, {"cron", "* * * * *"}, {"prompt", "hello"}});
    EXPECT_NE(result.find("Added cron job 'test'"), std::string::npos);
    EXPECT_TRUE(scheduler->has_job("test"));
    EXPECT_EQ(store->jobs().size(), 1);
}

TEST_F(CronToolTest, AddDuplicateFails) {
    exec({{"op", "add"}, {"name", "dupe"}, {"cron", "* * * * *"}, {"prompt", "a"}});
    auto result = exec({{"op", "add"}, {"name", "dupe"}, {"cron", "0 * * * *"}, {"prompt", "b"}});
    EXPECT_NE(result.find("already exists"), std::string::npos);
}

TEST_F(CronToolTest, AddInvalidCronFails) {
    auto result = exec({{"op", "add"}, {"name", "bad"}, {"cron", "invalid"}, {"prompt", "x"}});
    EXPECT_NE(result.find("invalid cron"), std::string::npos);
}

TEST_F(CronToolTest, RemoveDynamicJob) {
    exec({{"op", "add"}, {"name", "removable"}, {"cron", "* * * * *"}, {"prompt", "x"}});
    auto result = exec({{"op", "remove"}, {"name", "removable"}});
    EXPECT_NE(result.find("Removed"), std::string::npos);
    EXPECT_FALSE(scheduler->has_job("removable"));
}

TEST_F(CronToolTest, RemoveStaticJobFails) {
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler->add_job("static", *expr, "default", "cli", "prompt", false);

    auto result = exec({{"op", "remove"}, {"name", "static"}});
    EXPECT_NE(result.find("static (config) job"), std::string::npos);
}

TEST_F(CronToolTest, RemoveNonexistentFails) {
    auto result = exec({{"op", "remove"}, {"name", "ghost"}});
    EXPECT_NE(result.find("no job"), std::string::npos);
}

TEST_F(CronToolTest, ListShowsJobs) {
    exec({{"op", "add"}, {"name", "lister"}, {"cron", "*/5 * * * *"}, {"prompt", "check"}});
    auto result = exec({{"op", "list"}});
    EXPECT_NE(result.find("lister"), std::string::npos);
    EXPECT_NE(result.find("dynamic"), std::string::npos);
}

TEST_F(CronToolTest, ListEmptyShowsMessage) {
    auto result = exec({{"op", "list"}});
    EXPECT_NE(result.find("No cron jobs"), std::string::npos);
}

TEST_F(CronToolTest, RunFiresImmediately) {
    exec({{"op", "add"}, {"name", "runner"}, {"cron", "0 0 1 1 *"}, {"prompt", "go"}});
    auto result = exec({{"op", "run"}, {"name", "runner"}});
    EXPECT_NE(result.find("Fired job"), std::string::npos);
    EXPECT_EQ(fire_count, 1);
    EXPECT_EQ(last_fired_name, "runner");
}

TEST_F(CronToolTest, RunNonexistentFails) {
    auto result = exec({{"op", "run"}, {"name", "ghost"}});
    EXPECT_NE(result.find("no job"), std::string::npos);
}

TEST_F(CronToolTest, UnknownOpFails) {
    auto result = exec({{"op", "dance"}});
    EXPECT_NE(result.find("unknown operation"), std::string::npos);
}
