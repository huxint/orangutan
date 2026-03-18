#include "features/cron/store.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

class CronStoreTest : public ::testing::Test {
protected:
    std::string test_path;

    void SetUp() override {
        test_path = std::string(std::getenv("HOME")) + "/.orangutan/test-cron-store/jobs.json";
        std::filesystem::remove_all(std::filesystem::path(test_path).parent_path());
    }

    void TearDown() override {
        std::filesystem::remove_all(std::filesystem::path(test_path).parent_path());
    }
};

} // namespace

TEST_F(CronStoreTest, EmptyPathReturnsEmpty) {
    CronStore store("");
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, MissingFileReturnsEmpty) {
    CronStore store(test_path);
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, AddAndRetrieve) {
    CronStore store(test_path);
    EXPECT_TRUE(store.add({.name = "test-job", .cron = "* * * * *", .prompt = "Hello"}));
    EXPECT_EQ(store.jobs().size(), 1);
    EXPECT_EQ(store.jobs()[0].name, "test-job");
}

TEST_F(CronStoreTest, AddDuplicateFails) {
    CronStore store(test_path);
    store.add({.name = "dupe", .cron = "* * * * *", .prompt = "A"});
    EXPECT_FALSE(store.add({.name = "dupe", .cron = "0 * * * *", .prompt = "B"}));
    EXPECT_EQ(store.jobs().size(), 1);
}

TEST_F(CronStoreTest, RemoveExistingJob) {
    CronStore store(test_path);
    store.add({.name = "removable", .cron = "* * * * *", .prompt = "test"});
    EXPECT_TRUE(store.remove("removable"));
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, RemoveNonexistentFails) {
    CronStore store(test_path);
    EXPECT_FALSE(store.remove("ghost"));
}

TEST_F(CronStoreTest, SaveLoadRoundTrip) {
    {
        CronStore store(test_path);
        store.add({.name = "job-1", .cron = "0 9 * * *", .prompt = "morning check", .agent = "monitor", .channel = "slack"});
        store.add({.name = "job-2", .cron = "*/5 * * * *", .prompt = "health check"});
    }

    // Load from same path — should round-trip
    CronStore store2(test_path);
    EXPECT_EQ(store2.jobs().size(), 2);
    EXPECT_EQ(store2.jobs()[0].name, "job-1");
    EXPECT_EQ(store2.jobs()[0].agent, "monitor");
    EXPECT_EQ(store2.jobs()[0].channel, "slack");
    EXPECT_EQ(store2.jobs()[1].name, "job-2");
    EXPECT_EQ(store2.jobs()[1].agent, "default");
}

TEST_F(CronStoreTest, CorruptFileReturnsEmpty) {
    auto dir = std::filesystem::path(test_path).parent_path();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(test_path);
        f << "not valid json {{{";
    }

    CronStore store(test_path);
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, NotArrayReturnsEmpty) {
    auto dir = std::filesystem::path(test_path).parent_path();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(test_path);
        f << R"({"name": "not an array"})";
    }

    CronStore store(test_path);
    EXPECT_TRUE(store.jobs().empty());
}

TEST_F(CronStoreTest, AtomicWriteDoesNotLeaveTemp) {
    CronStore store(test_path);
    store.add({.name = "atomic-test", .cron = "* * * * *", .prompt = "test"});
    EXPECT_FALSE(std::filesystem::exists(test_path + ".tmp"));
    EXPECT_TRUE(std::filesystem::exists(test_path));
}
