#pragma once

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::providers {

    class SseParser {
    public:
        using EventCallback = std::function<void(const std::string &event, const std::string &data)>;

        explicit SseParser(EventCallback callback);
        void feed(std::string_view data);

    private:
        EventCallback callback_;
        std::string buffer_;
        std::string current_event_;
        std::string current_data_;

        void process_line(std::string_view line);
        void emit_event();
    };

    template <typename Derived>
    class JsonSseAccumulator {
    public:
        ~JsonSseAccumulator() = default;

        void handle_data(std::string_view data) {
            if (data == "[DONE]") {
                return;
            }

            if (auto payload = parse_payload(data, derived().parse_error_context()); payload.has_value()) {
                derived().handle_parsed_payload(*payload);
            }
        }

        void handle_event(std::string_view event_name, std::string_view data) {
            if (data == "[DONE]") {
                return;
            }

            if (auto payload = parse_payload(data, derived().parse_error_context()); payload.has_value()) {
                derived().handle_parsed_payload(event_name, *payload);
            }
        }

    private:
        friend Derived;

        JsonSseAccumulator() = default;
        JsonSseAccumulator(const JsonSseAccumulator &) = default;
        JsonSseAccumulator &operator=(const JsonSseAccumulator &) = default;
        JsonSseAccumulator(JsonSseAccumulator &&) = default;
        JsonSseAccumulator &operator=(JsonSseAccumulator &&) = default;
        [[nodiscard]]
        static std::optional<nlohmann::json> parse_payload(std::string_view data, std::string_view context) {
            try {
                return nlohmann::json::parse(data);
            } catch (const nlohmann::json::parse_error &error) {
                spdlog::warn("Failed to parse {}: {}", context, error.what());
                return std::nullopt;
            }
        }

        [[nodiscard]]
        Derived &derived() {
            return static_cast<Derived &>(*this);
        }
    };

} // namespace orangutan::providers

namespace orangutan {

    using providers::JsonSseAccumulator;
    using providers::SseParser;

} // namespace orangutan
