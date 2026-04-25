#pragma once

#include "automation/core-model.hpp"
#include "utils/transparent-lookup.hpp"

#include <exception>
#include <expected>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <utility>

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

    template <typename Payload>
    using ActionExecutor = std::function<ExecutorResult(const Payload &, const ExecutionContext &)>;

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

            std::scoped_lock lock(mutex_);
            auto [it, inserted] = entries_.try_emplace(
                std::move(key),
                Entry{
                    .payload_type = std::type_index(typeid(Payload)),
                });
            if (!inserted) {
                return std::unexpected("action key already registered");
            }
            return {};
        }

        template <typename Payload>
        auto register_action(std::string key, ActionExecutor<Payload> executor) -> ActionResult<void> {
            return register_action_impl<Payload>(std::move(key), std::move(executor));
        }

        [[nodiscard]]
        auto dispatch(const ActionDescriptor &action, const ExecutionContext &context) const -> ExecutorResult {
            std::function<ExecutorResult(const nlohmann::json &, const ExecutionContext &)> executor;
            {
                std::scoped_lock lock(mutex_);
                const auto it = utils::transparent_find(entries_, action.action_key);
                if (it == entries_.end()) {
                    return std::unexpected("action key is not registered");
                }
                if (!it->second.executor) {
                    return std::unexpected("action handler is not registered");
                }
                executor = it->second.executor;
            }

            return executor(action.payload, context);
        }

        [[nodiscard]]
        auto contains(std::string_view key) const -> bool {
            std::scoped_lock lock(mutex_);
            return utils::transparent_contains(entries_, key);
        }

        template <typename Payload>
        auto bind(std::string_view key, const Payload &payload) const -> ActionResult<ActionDescriptor> {
            {
                std::scoped_lock lock(mutex_);
                const auto it = utils::transparent_find(entries_, key);
                if (it == entries_.end()) {
                    return std::unexpected("action key is not registered");
                }
                if (it->second.payload_type != std::type_index(typeid(Payload))) {
                    return std::unexpected("payload type does not match registered action");
                }
            }

            auto encoded = encode_action_payload(payload);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }

            return ActionDescriptor{
                .action_key = std::string(key),
                .payload = std::move(*encoded),
            };
        }

    private:
        template <typename Payload>
        auto register_action_impl(std::string key, ActionExecutor<Payload> executor) -> ActionResult<void> {
            if (key.empty()) {
                return std::unexpected("action key must not be empty");
            }

            std::function<ExecutorResult(const nlohmann::json &, const ExecutionContext &)> erased_executor;
            if (executor) {
                erased_executor = [executor = std::move(executor)](const nlohmann::json &payload, const ExecutionContext &context) -> ExecutorResult {
                    try {
                        return executor(payload.template get<Payload>(), context);
                    } catch (const std::exception &exception) {
                        return std::unexpected(std::string(exception.what()));
                    }
                };
            }

            std::scoped_lock lock(mutex_);
            auto [it, inserted] = entries_.try_emplace(
                std::move(key),
                Entry{
                    .payload_type = std::type_index(typeid(Payload)),
                    .executor = std::move(erased_executor),
                });
            if (!inserted) {
                return std::unexpected("action key already registered");
            }
            return {};
        }

        struct Entry {
            std::type_index payload_type{typeid(void)};
            std::function<ExecutorResult(const nlohmann::json &, const ExecutionContext &)> executor;
        };

        mutable std::mutex mutex_;
        utils::transparent_string_unordered_map<Entry> entries_;
    };

} // namespace orangutan::automation
