#include "channel/qq/reconnect-backoff.hpp"
#include "channel/qq/qq-transport.hpp"
#include "utils/task-pool.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

using namespace orangutan;
using namespace orangutan::qq;

namespace {

    class FakeConnection final : public Transport::Connection {
    public:
        void send_text(std::string payload) override {
            {
                std::scoped_lock lock(mutex_);
                sent_payloads_.push_back(std::move(payload));
            }
            cv_.notify_all();
        }

        [[nodiscard]]
        std::optional<Transport::Event> wait_event(std::chrono::milliseconds timeout) override {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, timeout, [this] {
                return closed_ || !events_.empty();
            });

            if (!events_.empty()) {
                auto event = std::move(events_.front());
                events_.pop_front();
                return event;
            }
            if (closed_) {
                return Transport::Event::close(1000, "client close");
            }
            return std::nullopt;
        }

        void close() override {
            {
                std::scoped_lock lock(mutex_);
                closed_ = true;
            }
            cv_.notify_all();
        }

        void push_text(std::string payload) {
            {
                std::scoped_lock lock(mutex_);
                events_.push_back(Transport::Event::text(std::move(payload)));
            }
            cv_.notify_all();
        }

        void push_close(std::uint16_t code, std::string reason) {
            {
                std::scoped_lock lock(mutex_);
                events_.push_back(Transport::Event::close(code, std::move(reason)));
            }
            cv_.notify_all();
        }

        void push_error(std::string message) {
            {
                std::scoped_lock lock(mutex_);
                events_.push_back(Transport::Event::error(std::move(message)));
            }
            cv_.notify_all();
        }

        [[nodiscard]]
        bool wait_for_sent_count(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds(1)) const {
            std::unique_lock lock(mutex_);
            return cv_.wait_for(lock, timeout, [this, expected] {
                return sent_payloads_.size() >= expected;
            });
        }

        [[nodiscard]]
        std::vector<std::string> sent_payloads() const {
            std::scoped_lock lock(mutex_);
            return sent_payloads_;
        }

    private:
        mutable std::mutex mutex_;
        mutable std::condition_variable cv_;
        std::deque<Transport::Event> events_;
        std::vector<std::string> sent_payloads_;
        bool closed_ = false;
    };

    class FakeConnector {
    public:
        [[nodiscard]]
        Transport::ConnectionFactory factory() {
            return [this](const std::string &url) {
                std::scoped_lock lock(mutex_);
                urls_.push_back(url);
                if (failures_remaining_ > 0) {
                    --failures_remaining_;
                    throw std::runtime_error("synthetic connect failure");
                }

                auto connection = std::make_shared<FakeConnection>();
                connections_.push_back(connection);
                cv_.notify_all();
                return std::unique_ptr<Transport::Connection>(new SharedFakeConnection(connection));
            };
        }

        void fail_next_connects(std::size_t count) {
            std::scoped_lock lock(mutex_);
            failures_remaining_ = count;
        }

        [[nodiscard]]
        std::shared_ptr<FakeConnection> wait_for_connection(std::size_t index, std::chrono::milliseconds timeout = std::chrono::seconds(3)) const {
            std::unique_lock lock(mutex_);
            const auto ready = cv_.wait_for(lock, timeout, [this, index] {
                return connections_.size() >= index;
            });
            if (!ready) {
                throw std::runtime_error("timed out waiting for connection");
            }
            return connections_.at(index - 1);
        }

        [[nodiscard]]
        std::size_t connection_count() const {
            std::scoped_lock lock(mutex_);
            return connections_.size();
        }

    private:
        class SharedFakeConnection final : public Transport::Connection {
        public:
            explicit SharedFakeConnection(std::shared_ptr<FakeConnection> connection)
            : connection_(std::move(connection)) {}

            void send_text(std::string payload) override {
                connection_->send_text(std::move(payload));
            }

            [[nodiscard]]
            std::optional<Transport::Event> wait_event(std::chrono::milliseconds timeout) override {
                return connection_->wait_event(timeout);
            }

            void close() override {
                connection_->close();
            }

        private:
            std::shared_ptr<FakeConnection> connection_;
        };

        mutable std::mutex mutex_;
        mutable std::condition_variable cv_;
        std::vector<std::string> urls_;
        std::vector<std::shared_ptr<FakeConnection>> connections_;
        std::size_t failures_remaining_ = 0;
    };

    struct CallbackRecorder {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t opens = 0;
        std::vector<std::string> texts;
        std::vector<std::pair<std::uint16_t, std::string>> closes;
        std::vector<std::string> errors;

        [[nodiscard]]
        bool wait_for_opens(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [this, expected] {
                return opens >= expected;
            });
        }

        [[nodiscard]]
        bool wait_for_texts(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [this, expected] {
                return texts.size() >= expected;
            });
        }

        [[nodiscard]]
        bool wait_for_errors(std::size_t expected, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [this, expected] {
                return errors.size() >= expected;
            });
        }
    };

    [[nodiscard]]
    Transport make_transport(CallbackRecorder &recorder, FakeConnector &connector, utils::TaskPool &task_pool) {
        return Transport({
                             .on_open =
                                 [&] {
                                     std::scoped_lock lock(recorder.mutex);
                                     ++recorder.opens;
                                     recorder.cv.notify_all();
                                 },
                             .on_text =
                                 [&](std::string text) {
                                     std::scoped_lock lock(recorder.mutex);
                                     recorder.texts.push_back(std::move(text));
                                     recorder.cv.notify_all();
                                 },
                             .on_close =
                                 [&](std::uint16_t code, std::string reason) {
                                     std::scoped_lock lock(recorder.mutex);
                                     recorder.closes.emplace_back(code, std::move(reason));
                                     recorder.cv.notify_all();
                                 },
                             .on_error =
                                 [&](std::string error) {
                                     std::scoped_lock lock(recorder.mutex);
                                     recorder.errors.push_back(std::move(error));
                                     recorder.cv.notify_all();
                                 },
                         },
                         connector.factory(), task_pool);
    }

} // namespace

TEST_CASE("reconnect_backoff_uses_capped_exponential_sequence") {
    ReconnectBackoff backoff;

    const auto first = backoff.next_delay();
    const auto second = backoff.next_delay();
    const auto third = backoff.next_delay();
    const auto fourth = backoff.next_delay();
    const auto fifth = backoff.next_delay();
    const auto sixth = backoff.next_delay();

    CHECK(first == std::chrono::seconds(1));
    CHECK(second == std::chrono::seconds(2));
    CHECK(third == std::chrono::seconds(4));
    CHECK(fourth == std::chrono::seconds(8));
    CHECK(fifth == std::chrono::seconds(15));
    CHECK(sixth == std::chrono::seconds(15));
};

TEST_CASE("reconnect_backoff_reset_restarts_sequence") {
    ReconnectBackoff backoff;

    const auto first = backoff.next_delay();
    const auto second = backoff.next_delay();

    backoff.reset();

    const auto reset_first = backoff.next_delay();

    CHECK(first == std::chrono::seconds(1));
    CHECK(second == std::chrono::seconds(2));
    CHECK(reset_first == std::chrono::seconds(1));
};

TEST_CASE("transport_start_connects_and_forwards_messages") {
    FakeConnector connector;
    CallbackRecorder recorder;
    utils::TaskPool task_pool;
    auto transport = make_transport(recorder, connector, task_pool);

    transport.start("wss://qq.example/ws");
    auto connection = connector.wait_for_connection(1);
    REQUIRE(recorder.wait_for_opens(1));

    transport.send_text("ping");
    REQUIRE(connection->wait_for_sent_count(1));
    CHECK(connection->sent_payloads().at(0) == "ping");

    connection->push_text("gateway payload");
    REQUIRE(recorder.wait_for_texts(1));
    CHECK(recorder.texts.at(0) == "gateway payload");

    transport.stop();
};

TEST_CASE("transport_open_connection_does_not_starve_caller_task_pool") {
    FakeConnector connector;
    CallbackRecorder recorder;
    utils::TaskPool task_pool;
    auto transport = make_transport(recorder, connector, task_pool);

    transport.start("wss://qq.example/ws");
    static_cast<void>(connector.wait_for_connection(1));
    REQUIRE(recorder.wait_for_opens(1));

    std::mutex mutex;
    std::condition_variable cv;
    bool unrelated_task_ran = false;

    stdexec::start_detached(stdexec::schedule(task_pool.scheduler()) | stdexec::then([&] {
                                {
                                    std::scoped_lock lock(mutex);
                                    unrelated_task_ran = true;
                                }
                                cv.notify_all();
                            }));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
        return unrelated_task_ran;
    }));

    transport.stop();
};

TEST_CASE("transport_connect_failure_emits_error_and_reconnects") {
    FakeConnector connector;
    connector.fail_next_connects(1);
    CallbackRecorder recorder;
    utils::TaskPool task_pool;
    auto transport = make_transport(recorder, connector, task_pool);

    transport.start("wss://qq.example/ws");

    REQUIRE(recorder.wait_for_errors(1));
    auto connection = connector.wait_for_connection(1, std::chrono::seconds(5));
    REQUIRE(recorder.wait_for_opens(1, std::chrono::seconds(5)));
    REQUIRE(connection != nullptr);
    CHECK(connector.connection_count() == 1UL);
    CHECK(recorder.errors.size() == 1UL);

    transport.stop();
};

TEST_CASE("transport_request_reconnect_suppresses_close_callback_and_reopens") {
    FakeConnector connector;
    CallbackRecorder recorder;
    utils::TaskPool task_pool;
    auto transport = make_transport(recorder, connector, task_pool);

    transport.start("wss://qq.example/ws");
    auto first = connector.wait_for_connection(1);
    REQUIRE(recorder.wait_for_opens(1));

    transport.request_reconnect();

    auto second = connector.wait_for_connection(2, std::chrono::seconds(5));
    REQUIRE(recorder.wait_for_opens(2, std::chrono::seconds(5)));
    REQUIRE(first != second);
    CHECK(recorder.closes.empty());

    transport.stop();
};

TEST_CASE("transport_stop_prevents_further_sends_and_callbacks") {
    FakeConnector connector;
    CallbackRecorder recorder;
    utils::TaskPool task_pool;
    auto transport = make_transport(recorder, connector, task_pool);

    transport.start("wss://qq.example/ws");
    auto connection = connector.wait_for_connection(1);
    REQUIRE(recorder.wait_for_opens(1));

    transport.stop();

    REQUIRE_THROWS_AS(transport.send_text("after-stop"), std::runtime_error);

    connection->push_error("late failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(recorder.errors.empty());
};
