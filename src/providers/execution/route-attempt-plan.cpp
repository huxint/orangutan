#include "providers/execution/route-attempt-plan.hpp"

#include <unordered_set>

#include "utils/enum-string.hpp"

namespace orangutan::providers::execution {

    std::string target_key(const ModelTarget &target) {
        return target.profile_name + "|" + std::string(utils::enum_name(target.provider)) + "|" + std::string(utils::enum_name_kebab(target.protocol)) + "|" +
               target.model + "|" + target.base_url;
    }

    namespace {

        [[nodiscard]]
        std::vector<ModelTarget> flatten_route(const ProviderRoute &route) {
            std::vector<ModelTarget> targets;
            targets.reserve(1 + route.fallbacks.size());

            std::unordered_set<std::string> seen;
            const auto append = [&targets, &seen](const ModelTarget &target) {
                if (target.model.empty()) {
                    return;
                }
                if (!seen.insert(target_key(target)).second) {
                    return;
                }
                targets.push_back(target);
            };

            append(route.primary);
            for (const auto &fallback : route.fallbacks) {
                append(fallback);
            }

            return targets;
        }

        [[nodiscard]]
        std::size_t starting_index(std::span<const ModelTarget> targets, std::string_view preferred_target_key) {
            if (preferred_target_key.empty()) {
                return 0;
            }

            for (std::size_t index = 0; index < targets.size(); ++index) {
                if (target_key(targets[index]) == preferred_target_key) {
                    return index;
                }
            }

            return 0;
        }

    } // namespace

    RouteAttemptPlan::RouteAttemptPlan(const ProviderRoute &route, std::string_view preferred_target_key)
    : targets_(flatten_route(route)),
      current_index_(starting_index(targets_, preferred_target_key)) {}

    bool RouteAttemptPlan::empty() const {
        return targets_.empty();
    }

    const ModelTarget &RouteAttemptPlan::current() const {
        return targets_[current_index_];
    }

    bool RouteAttemptPlan::can_advance_after(const ProviderError &error, bool emitted_stream_event) const {
        return error.retryable() && !emitted_stream_event && current_index_ + 1 < targets_.size();
    }

    void RouteAttemptPlan::advance() {
        ++current_index_;
    }

    std::size_t RouteAttemptPlan::current_index() const {
        return current_index_;
    }

    std::span<const ModelTarget> RouteAttemptPlan::targets() const {
        return targets_;
    }

} // namespace orangutan::providers::execution
