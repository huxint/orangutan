#include "features/cron/parser.hpp"
#include "features/cron/store.hpp"
#include "features/heartbeat/scheduler.hpp"
#include "features/tools/builtin/cron.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;
using orangutan::testing::test_tmp_root;

namespace {

class CronToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        store_path_ = (test_tmp_root() / "test-cron-tool" / "jobs.json").string();
        std::filesystem::remove_all(std::filesystem::path(store_path_).parent_path());

        store_ = std::make_unique<CronStore>(store_path_);
        scheduler_ = std::make_unique<HeartbeatScheduler>([this](const HeartbeatJob &job) {
            ++fire_count_;
            last_fired_name_ = job.name;
        });

        ctx_.cron_store = store_.get();
        ctx_.heartbeat_scheduler = scheduler_.get();
        register_cron_tool(registry_, &ctx_);
    }

    void TearDown() override {
        tmp_env_.reset();
        std::filesystem::remove_all(std::filesystem::path(store_path_).parent_path());
    }

    std::string exec(const json &input) {
        ToolUseBlock call;
        call.name = "cron";
        call.input = input;
        auto result = registry_.execute(call);
        return result.content;
    }

    [[nodiscard]]
    const std::string &store_path() const {
        return store_path_;
    }

    [[nodiscard]]
    CronStore &store() const {
        return *store_;
    }

    [[nodiscard]]
    HeartbeatScheduler &scheduler() const {
        return *scheduler_;
    }

    [[nodiscard]]
    int fire_count() const {
        return fire_count_;
    }

    [[nodiscard]]
    const std::string &last_fired_name() const {
        return last_fired_name_;
    }

private:
    std::string store_path_;
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::unique_ptr<CronStore> store_;
    std::unique_ptr<HeartbeatScheduler> scheduler_;
    ToolRuntimeContext ctx_{};
    ToolRegistry registry_;
    int fire_count_ = 0;
    std::string last_fired_name_;
};

} // namespace

TEST_F(CronToolTest, AddCreatesJob) {
    auto result = exec({{"op", "add"}, {"name", "test"}, {"cron", "* * * * *"}, {"prompt", "hello"}});
    EXPECT_NE(result.find("Added cron job 'test'"), std::string::npos);
    EXPECT_TRUE(scheduler().has_job("test"));
    EXPECT_EQ(store().jobs().size(), 1);
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
    EXPECT_FALSE(scheduler().has_job("removable"));
}

TEST_F(CronToolTest, RemoveRollbackRestoresSchedulerWhenPersistenceFails) {
    exec({{"op", "add"}, {"name", "removable"}, {"cron", "* * * * *"}, {"prompt", "x"}});

    const auto blocked_root = test_tmp_root() / "test-cron-tool-blocked";
    std::filesystem::remove_all(blocked_root);
    std::filesystem::rename(std::filesystem::path(store_path()).parent_path(), blocked_root);
    std::ofstream(std::filesystem::path(store_path()).parent_path()) << "not a directory";

    const auto result = exec({{"op", "remove"}, {"name", "removable"}});
    EXPECT_NE(result.find("failed to persist removal"), std::string::npos);
    EXPECT_TRUE(scheduler().has_job("removable"));
    ASSERT_EQ(store().jobs().size(), 1);
    EXPECT_EQ(store().jobs()[0].name, "removable");

    std::filesystem::remove(std::filesystem::path(store_path()).parent_path());
    std::filesystem::rename(blocked_root, std::filesystem::path(store_path()).parent_path());
}

TEST_F(CronToolTest, RemoveStaticJobFails) {
    auto expr = parse_cron("* * * * *");
    ASSERT_TRUE(expr.has_value());
    scheduler().add_job("static", *expr, "default", "cli", "prompt", false);

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
    EXPECT_EQ(fire_count(), 1);
    EXPECT_EQ(last_fired_name(), "runner");
}

TEST_F(CronToolTest, RunNonexistentFails) {
    auto result = exec({{"op", "run"}, {"name", "ghost"}});
    EXPECT_NE(result.find("no job"), std::string::npos);
}

TEST_F(CronToolTest, UnknownOpFails) {
    auto result = exec({{"op", "dance"}});
    EXPECT_NE(result.find("unknown operation"), std::string::npos);
}
