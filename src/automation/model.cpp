#include "automation/model.hpp"

#include "automation/builder.hpp"

#include <array>
#include <random>

#include <magic_enum/magic_enum.hpp>

namespace orangutan::automation {
    namespace {

        [[nodiscard]]
        std::string random_hex(std::size_t count) {
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            static constexpr std::array<char, 16> HEX = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

            std::string value;
            value.reserve(count);
            std::uniform_int_distribution<int> dist(0, 15);
            for (std::size_t index = 0; index < count; ++index) {
                value.push_back(HEX.at(static_cast<std::size_t>(dist(rng))));
            }
            return value;
        }

    } // namespace

    std::string generate_id(std::string_view prefix) {
        const auto now = to_unix_seconds(Clock::now());
        return std::string(prefix) + "-" + std::to_string(now) + "-" + random_hex(10);
    }

    base::i64 to_unix_seconds(TimePoint time) {
        return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    TimePoint from_unix_seconds(base::i64 seconds) {
        return TimePoint{std::chrono::seconds(seconds)};
    }

    nlohmann::json delivery_policy_to_json(const DeliveryPolicy &delivery) {
        return {
            {"mode", magic_enum::enum_name(delivery.mode)},
            {"targets", delivery.targets},
        };
    }

    DeliveryPolicy delivery_policy_from_json(const nlohmann::json &value) {
        DeliveryPolicy delivery;
        if (!value.is_object()) {
            return delivery;
        }

        if (const auto it = value.find("mode"); it != value.end() && it->is_string()) {
            if (const auto parsed = magic_enum::enum_cast<delivery_mode>(it->get_ref<const std::string &>()); parsed.has_value()) {
                delivery.mode = *parsed;
            }
        }

        if (const auto it = value.find("targets"); it != value.end() && it->is_array()) {
            for (const auto &item : *it) {
                if (item.is_string()) {
                    delivery.targets.push_back(item.get<std::string>());
                }
            }
        }

        return delivery;
    }

    AutomationBuilder Automation::named(std::string_view name) {
        return AutomationBuilder{name};
    }

} // namespace orangutan::automation
