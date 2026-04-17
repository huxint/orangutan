#include "web/errors.hpp"
#include "web/event-bus.hpp"
#include "web/sse.hpp"
#include "web/web-routes.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>

namespace orangutan::web {

    namespace {

        /// Parse `Last-Event-ID` header (or `?last_event_id=` param) so reconnecting
        /// clients replay everything they missed without gaps.
        std::uint64_t parse_last_event_id(const httplib::Request &req) {
            std::string value;
            if (req.has_header("Last-Event-ID")) {
                value = req.get_header_value("Last-Event-ID");
            } else if (req.has_param("last_event_id")) {
                value = req.get_param_value("last_event_id");
            }
            if (value.empty()) {
                return 0;
            }
            std::uint64_t result = 0;
            const auto *begin = value.data();
            const auto *end = begin + value.size(); // NOLINT: bounded by string length
            const auto [ptr, ec] = std::from_chars(begin, end, result);
            if (ec != std::errc{}) {
                return 0;
            }
            return result;
        }

        /// Thread-safe hand-off queue between the publisher thread (EventBus)
        /// and the HTTP stream's drain loop. Shared via shared_ptr so the
        /// subscription handler outlives any one iteration of the stream.
        struct StreamQueue {
            std::mutex mutex;
            std::condition_variable cv;
            std::queue<BusEvent> events;
            std::atomic<bool> closed{false};
        };

    } // namespace

    void handle_event_stream(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.event_bus == nullptr) {
            send_error(res, 503, "event_bus_unavailable", "event bus not wired");
            return;
        }

        const auto last_event_id = parse_last_event_id(req);
        auto queue = std::make_shared<StreamQueue>();

        // Own the subscription via shared_ptr so it drops exactly when the last
        // capture of the stream lambda dies.
        auto subscription = std::make_shared<EventBus::Subscription>(ctx.event_bus->subscribe(
            [queue](const BusEvent &event) {
                std::scoped_lock lock(queue->mutex);
                queue->events.push(event);
                queue->cv.notify_one();
            },
            last_event_id));

        prepare_sse_response(res);
        res.set_chunked_content_provider("text/event-stream", [queue, subscription](std::size_t /*offset*/, httplib::DataSink &sink) -> bool {
            // Tell the client the stream is alive and hand it the starting sequence so
            // the frontend can pick up where it left off.
            write_sse(sink, "ready", {{"status", "connected"}});

            using namespace std::chrono_literals;
            while (!queue->closed.load()) {
                std::unique_lock lock(queue->mutex);
                queue->cv.wait_for(lock, 15s, [&queue] {
                    return !queue->events.empty() || queue->closed.load();
                });

                if (queue->closed.load()) {
                    break;
                }

                if (queue->events.empty()) {
                    lock.unlock();
                    if (!write_sse_keepalive(sink)) {
                        break;
                    }
                    continue;
                }

                BusEvent event = std::move(queue->events.front());
                queue->events.pop();
                lock.unlock();

                nlohmann::json envelope = {
                    {"kind", event.kind},
                    {"scope", event.scope},
                    {"payload", event.payload},
                    {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(event.timestamp.time_since_epoch()).count()},
                };
                if (!write_sse(sink, event.kind, envelope, std::to_string(event.sequence))) {
                    break;
                }

                if (sink.is_writable != nullptr && !sink.is_writable()) {
                    break;
                }
            }

            queue->closed.store(true);
            sink.done();
            return false;
        });
    }

} // namespace orangutan::web
