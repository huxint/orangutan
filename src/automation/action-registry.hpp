#pragma once

#include "automation/core-model.hpp"
#include "utils/transparent-lookup.hpp"

#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <typeindex>

namespace orangutan::automation {

    using ActionError = std::string;

    template <typename T>
    using ActionResult = std::expected<T, ActionError>;

    struct ExecutionContext {
        JobId job_id;
        ExecutionId execution_id;
        TimePoint scheduled_for{};
        std::stop_token stop_token{};
    };

    using ExecutorResult = std::expected<ExecutionResult, std::string>;

    class ExecutorPort {
    public:
        virtual ~ExecutorPort() = default;

        ExecutorPort(const ExecutorPort &) = delete;
        auto operator=(const ExecutorPort &) -> ExecutorPort & = delete;
        ExecutorPort(ExecutorPort &&) = delete;
        auto operator=(ExecutorPort &&) -> ExecutorPort & = delete;

        [[nodiscard]]
        virtual auto dispatch(const DispatchRequest &request, const ExecutionContext &context) -> ExecutorResult = 0;

    protected:
        ExecutorPort() = default;
    };

    template <typename Payload>
    auto encode_action_payload(const Payload &payload) -> ActionResult<nlohmann::json> {
        try {
            return nlohmann::json(payload);
        } catch (const std::exception &exception) {
            return std::unexpected(std::string(exception.what()));
        }
    }

    class ActionRegistry {
    public:
        ActionRegistry() = default;

        ActionRegistry(const ActionRegistry &) = delete;
        auto operator=(const ActionRegistry &) -> ActionRegistry & = delete;
        ActionRegistry(ActionRegistry &&) = delete;
        auto operator=(ActionRegistry &&) -> ActionRegistry & = delete;
        ~ActionRegistry() = default;

        template <typename Payload>
        auto register_action(std::string key) -> ActionResult<void> {
            if (key.empty()) {
                return std::unexpected("action key must not be empty");
            }

            auto [it, inserted] = entries_.try_emplace(
                std::move(key),
                Entry{
                    .payload_type = std::type_index(typeid(Payload)),
                    .encode =
                        [](const void *raw_payload) -> ActionResult<nlohmann::json> {
                        if (raw_payload == nullptr) {
                            return std::unexpected("payload pointer must not be null");
                        }
                        return encode_action_payload(*static_cast<const Payload *>(raw_payload));
                    },
                });
            if (!inserted) {
                return std::unexpected("action key already registered");
            }
            return {};
        }

        [[nodiscard]]
        auto contains(std::string_view key) const -> bool {
            return utils::transparent_contains(entries_, key);
        }

        template <typename Payload>
        auto bind(std::string_view key, const Payload &payload) const -> ActionResult<ActionDescriptor> {
            const auto it = utils::transparent_find(entries_, key);
            if (it == entries_.end()) {
                return std::unexpected("action key is not registered");
            }
            if (it->second.payload_type != std::type_index(typeid(Payload))) {
                return std::unexpected("payload type does not match registered action");
            }

            auto encoded = it->second.encode(std::addressof(payload));
            if (!encoded) {
                return std::unexpected(encoded.error());
            }

            return ActionDescriptor{
                .action_key = std::string(key),
                .payload = std::move(*encoded),
            };
        }

    private:
        struct Entry {
            std::type_index payload_type{typeid(void)};
            std::function<ActionResult<nlohmann::json>(const void *)> encode;
        };

        utils::transparent_string_unordered_map<Entry> entries_;
    };

} // namespace orangutan::automation
