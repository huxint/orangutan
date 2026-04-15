#pragma once

#include <cstdint>
#include <functional>
#include <ranges>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "tool-context.hpp"
#include "tool-registry.hpp"
#include "tool-spec-builder.hpp"

namespace orangutan::tools {

    enum class context_gate : std::uint8_t {
        automation_service,
        automation_runtime,
        channel_origin,
    };

    namespace detail {

        class ContextGateEvaluator {
        public:
            [[nodiscard]]
            static bool evaluate(context_gate gate, const ToolRuntimeContext &context, base::origin required_origin = base::origin::cli) {
                switch (gate) {
                    case context_gate::automation_service:
                        return context.automation_service != nullptr;
                    case context_gate::automation_runtime:
                        return context.automation_runtime != nullptr;
                    case context_gate::channel_origin:
                        return context.runtime_origin == required_origin;
                }

                return false;
            }
        };

    } // namespace detail

    class ContextualToolGroup {
    public:
        using Gate = std::function<bool(const ToolRuntimeContext &)>;

        auto when(this auto &&self, Gate gate) -> decltype(auto) {
            self.gates_.push_back(std::move(gate));
            return std::forward<decltype(self)>(self);
        }

        auto require_automation_runtime(this auto &&self) -> decltype(auto) {
            return std::forward<decltype(self)>(self).when([](const ToolRuntimeContext &ctx) {
                return detail::ContextGateEvaluator::evaluate(context_gate::automation_runtime, ctx);
            });
        }

        auto require_automation_service(this auto &&self) -> decltype(auto) {
            return std::forward<decltype(self)>(self).when([](const ToolRuntimeContext &ctx) {
                return detail::ContextGateEvaluator::evaluate(context_gate::automation_service, ctx);
            });
        }

        auto require_channel_origin(this auto &&self, base::origin origin) -> decltype(auto) {
            return std::forward<decltype(self)>(self).when([origin](const ToolRuntimeContext &ctx) {
                return detail::ContextGateEvaluator::evaluate(context_gate::channel_origin, ctx, origin);
            });
        }

        auto add(this auto &&self, ToolSpecBuilder spec) -> decltype(auto) {
            self.specs_.push_back(std::move(spec));
            return std::forward<decltype(self)>(self);
        }

        void register_into(ToolRegistry &registry, const ToolRuntimeContext *tool_context) const {
            const ToolRuntimeContext default_context{};
            const ToolRuntimeContext &context = (tool_context != nullptr) ? *tool_context : default_context;
            const bool allowed = std::ranges::all_of(gates_, [&context](const Gate &gate) {
                return gate(context);
            });

            if (!allowed) {
                return;
            }

            for (const auto &spec : specs_) {
                auto result = spec.build();
                if (result.has_value()) {
                    registry.register_tool(std::move(result).value());
                } else {
                    spdlog::warn("failed to register tool: {}", result.error());
                }
            }
        }

    private:
        std::vector<Gate> gates_;
        std::vector<ToolSpecBuilder> specs_;
    };

    using contextual_tool_group = ContextualToolGroup;

} // namespace orangutan::tools
