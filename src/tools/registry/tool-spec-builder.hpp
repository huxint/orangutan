#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "tool-registry.hpp"

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
    inline ToolSpecBuilder make_tool_spec_builder(std::string name) {
        return ToolSpecBuilder(std::move(name));
    }

    [[nodiscard]]
    inline ToolSpecBuilder tool_spec_builder(std::string name) {
        return make_tool_spec_builder(std::move(name));
    }

} // namespace orangutan::tools
