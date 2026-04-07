#pragma once

#include "tool-context.hpp"
#include "tool-registry.hpp"

#include <functional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace orangutan::tools {

    class ToolSpecBuilder {
    public:
        explicit ToolSpecBuilder(std::string name) {
            tool_.definition.name = std::move(name);
        }

        ToolSpecBuilder &description(std::string value) {
            tool_.definition.description = std::move(value);
            return *this;
        }

        ToolSpecBuilder &input_schema(nlohmann::json schema) {
            tool_.definition.input_schema = std::move(schema);
            return *this;
        }

        ToolSpecBuilder &read_only(bool value = true) {
            tool_.read_only = value;
            return *this;
        }

        ToolSpecBuilder &deferred(bool value = true) {
            tool_.deferred = value;
            return *this;
        }

        ToolSpecBuilder &check_permissions(std::function<PermissionResult(const ToolUse &, const ToolPermissionContext &)> fn) {
            tool_.check_permissions = std::move(fn);
            return *this;
        }

        ToolSpecBuilder &execute(std::function<std::string(const nlohmann::json &)> fn) {
            tool_.execute = std::move(fn);
            has_execute_ = tool_.execute != nullptr;
            return *this;
        }

        ToolSpecBuilder &execute_rich(std::function<ToolOutput(const nlohmann::json &)> fn) {
            tool_.execute_rich = std::move(fn);
            has_execute_rich_ = tool_.execute_rich != nullptr;
            return *this;
        }

        [[nodiscard]]
        Tool build() const {
            if (!has_execute_ && !has_execute_rich_) {
                throw std::invalid_argument("tool spec builder requires execute or execute_rich");
            }
            return tool_;
        }

    private:
        Tool tool_;
        bool has_execute_ = false;
        bool has_execute_rich_ = false;
    };

    [[nodiscard]]
    inline ToolSpecBuilder tool_spec_builder(std::string name) {
        return ToolSpecBuilder(std::move(name));
    }

    class contextual_tool_group {
    public:
        using Gate = std::function<bool(const ToolRuntimeContext &)>;

        contextual_tool_group &when(Gate gate) {
            gates_.push_back(std::move(gate));
            return *this;
        }

        contextual_tool_group &require_automation_runtime() {
            return when([](const ToolRuntimeContext &ctx) {
                return ctx.automation_runtime != nullptr;
            });
        }

        contextual_tool_group &require_channel_origin(base::origin origin) {
            return when([origin](const ToolRuntimeContext &ctx) {
                return ctx.runtime_origin == origin;
            });
        }

        contextual_tool_group &add(ToolSpecBuilder spec) {
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

    class tool_dispatch {
    public:
        struct response {
            std::string message;
            bool is_error = false;
        };

        using handler = std::function<response(const nlohmann::json &)>;
        using missing_op_error_formatter_fn = std::function<std::string(std::string_view)>;

        tool_dispatch &op_field(std::string field_name) {
            op_field_ = std::move(field_name);
            return *this;
        }

        tool_dispatch &unknown_op_error(std::string message) {
            unknown_op_error_ = std::move(message);
            return *this;
        }

        tool_dispatch &missing_op_error_formatter(missing_op_error_formatter_fn formatter) {
            missing_op_error_formatter_ = std::move(formatter);
            return *this;
        }

        tool_dispatch &on(std::string op, handler fn) {
            handlers_.insert_or_assign(std::move(op), std::move(fn));
            return *this;
        }

        [[nodiscard]]
        response run(const nlohmann::json &input) const {
            if (!input.contains(op_field_) || !input.at(op_field_).is_string()) {
                return {.message = missing_op_error_formatter_(op_field_), .is_error = true};
            }

            const auto op = input.at(op_field_).get<std::string>();
            const auto it = handlers_.find(op);
            if (it == handlers_.end()) {
                return {.message = unknown_op_error_, .is_error = true};
            }

            return it->second(input);
        }

    private:
        std::string op_field_ = "op";
        std::string unknown_op_error_ = "unknown operation";
        missing_op_error_formatter_fn missing_op_error_formatter_ = [](std::string_view field_name) {
            return "missing required field: " + std::string(field_name);
        };
        std::unordered_map<std::string, handler> handlers_;
    };

} // namespace orangutan::tools
