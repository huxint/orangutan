#include "features/cron/store.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;
using orangutan::testing::test_tmp_root;

namespace {

class CronStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", test_tmp_root().string());
        test_path_ = (test_tmp_root() / "test-cron-store" / "jobs.json").string();
        std::filesystem::remove_all(std::filesystem::path(test_path_).parent_path());
    }

    void TearDown() override {
        tmp_env_.reset();
        std::filesystem::remove_all(std::filesystem::path(test_path_).parent_path());
    }

    [[nodiscard]]
    const std::string &test_path() const {
        return test_path_;
    }

private:
    std::string test_path_;
    std::unique_ptr<ScopedEnvVar> tmp_env_;
};

} // namespace

TEST_F(CronStoreTest, EmptyPathReturnsEmpty) {
    CronStore store("");
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, MissingFileReturnsEmpty) {
    CronStore store(test_path());
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, AddAndRetrieve) {
    CronStore store(test_path());
    EXPECT_TRUE(store.add({.name = "test-job", .cron = "* * * * *", .prompt = "Hello"}));
    EXPECT_EQ(store.jobs().size(), 1);
    EXPECT_EQ(store.jobs()[0].name, "test-job");
}

TEST_F(CronStoreTest, AddDuplicateFails) {
    CronStore store(test_path());
    store.add({.name = "dupe", .cron = "* * * * *", .prompt = "A"});
    EXPECT_FALSE(store.add({.name = "dupe", .cron = "0 * * * *", .prompt = "B"}));
    EXPECT_EQ(store.jobs().size(), 1);
}

TEST_F(CronStoreTest, RemoveExistingJob) {
    CronStore store(test_path());
    store.add({.name = "removable", .cron = "* * * * *", .prompt = "test"});
    EXPECT_TRUE(store.remove("removable"));
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, RemoveNonexistentFails) {
    CronStore store(test_path());
    EXPECT_FALSE(store.remove("ghost"));
}

TEST_F(CronStoreTest, SaveLoadRoundTrip) {
    {
        CronStore store(test_path());
        store.add({.name = "job-1", .cron = "0 9 * * *", .prompt = "morning check", .agent = "monitor", .channel = "slack"});
        store.add({.name = "job-2", .cron = "*/5 * * * *", .prompt = "health check"});
    }

    // Load from same path — should round-trip
    CronStore store2(test_path());
    EXPECT_EQ(store2.jobs().size(), 2);
    EXPECT_EQ(store2.jobs()[0].name, "job-1");
    EXPECT_EQ(store2.jobs()[0].agent, "monitor");
    EXPECT_EQ(store2.jobs()[0].channel, "slack");
    EXPECT_EQ(store2.jobs()[1].name, "job-2");
    EXPECT_EQ(store2.jobs()[1].agent, "default");
}

TEST_F(CronStoreTest, CorruptFileReturnsEmpty) {
    auto dir = std::filesystem::path(test_path()).parent_path();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(test_path());
        f << "not valid json {{{";
    }

    CronStore store(test_path());
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, NotArrayReturnsEmpty) {
    auto dir = std::filesystem::path(test_path()).parent_path();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(test_path());
        f << R"({"name": "not an array"})";
    }

    CronStore store(test_path());
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, AtomicWriteDoesNotLeaveTemp) {
    CronStore store(test_path());
    store.add({.name = "atomic-test", .cron = "* * * * *", .prompt = "test"});
    EXPECT_FALSE(std::filesystem::exists(test_path() + ".tmp"));
    EXPECT_TRUE(std::filesystem::exists(test_path()));
}

TEST_F(CronStoreTest, AddFailureDoesNotMutateInMemoryJobs) {
    const auto blocked_root = test_tmp_root() / "test-cron-store-blocked";
    std::filesystem::remove_all(blocked_root);
    std::ofstream(blocked_root) << "not a directory";

    CronStore store((blocked_root / "jobs.json").string());
    EXPECT_FALSE(store.add({.name = "blocked", .cron = "* * * * *", .prompt = "test"}));
    EXPECT_TRUE(store.jobs().empty());

    std::filesystem::remove(blocked_root);
}

TEST_F(CronStoreTest, RemoveFailureDoesNotMutateInMemoryJobs) {
    CronStore store(test_path());
    ASSERT_TRUE(store.add({.name = "kept", .cron = "* * * * *", .prompt = "test"}));

    const auto blocked_root = test_tmp_root() / "test-cron-store-blocked-remove";
    std::filesystem::remove_all(blocked_root);
    std::filesystem::rename(std::filesystem::path(test_path()).parent_path(), blocked_root);
    std::ofstream(std::filesystem::path(test_path()).parent_path()) << "not a directory";

    EXPECT_FALSE(store.remove("kept"));
    ASSERT_EQ(store.jobs().size(), 1);
    EXPECT_EQ(store.jobs()[0].name, "kept");

    std::filesystem::remove(std::filesystem::path(test_path()).parent_path());
    std::filesystem::rename(blocked_root, std::filesystem::path(test_path()).parent_path());
}
