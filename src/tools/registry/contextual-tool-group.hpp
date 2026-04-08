#pragma once

#include <cstdint>
#include <functional>
#include <ranges>
#include <utility>
#include <vector>

#include "tool-context.hpp"
#include "tool-registry.hpp"
#include "tool-spec-builder.hpp"

namespace orangutan::tools {

    enum class context_gate : std::uint8_t {
        automation_runtime,
        channel_origin,
    };

    namespace detail {

        class ContextGateEvaluator {
        public:
            [[nodiscard]]
            static bool evaluate(context_gate gate, const ToolRuntimeContext &context, base::origin required_origin = base::origin::cli) {
                switch (gate) {
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

        ContextualToolGroup &when(Gate gate) {
            gates_.push_back(std::move(gate));
            return *this;
        }

        ContextualToolGroup &require_automation_runtime() {
            return when([](const ToolRuntimeContext &ctx) {
                return detail::ContextGateEvaluator::evaluate(context_gate::automation_runtime, ctx);
            });
        }

        ContextualToolGroup &require_channel_origin(base::origin origin) {
            return when([origin](const ToolRuntimeContext &ctx) {
                return detail::ContextGateEvaluator::evaluate(context_gate::channel_origin, ctx, origin);
            });
        }

        ContextualToolGroup &add(ToolSpecBuilder spec) {
            specs_.push_back(std::move(spec));
            return *this;
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
                registry.register_tool(spec.build());
            }
        }

    private:
        std::vector<Gate> gates_;
        std::vector<ToolSpecBuilder> specs_;
    };

    using contextual_tool_group = ContextualToolGroup;

} // namespace orangutan::tools
