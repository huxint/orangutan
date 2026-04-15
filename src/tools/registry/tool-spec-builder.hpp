#pragma once

#include <expected>
#include <functional>
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

        auto description(this auto &&self, std::string value) -> decltype(auto) {
            self.tool_.definition.description = std::move(value);
            return std::forward<decltype(self)>(self);
        }

        auto input_schema(this auto &&self, nlohmann::json schema) -> decltype(auto) {
            self.tool_.definition.input_schema = std::move(schema);
            return std::forward<decltype(self)>(self);
        }

        auto read_only(this auto &&self, bool value = true) -> decltype(auto) {
            self.tool_.read_only = value;
            return std::forward<decltype(self)>(self);
        }

        auto deferred(this auto &&self, bool value = true) -> decltype(auto) {
            self.tool_.deferred = value;
            return std::forward<decltype(self)>(self);
        }

        auto check_permissions(this auto &&self, std::function<PermissionResult(const ToolUse &, const ToolPermissionContext &)> fn) -> decltype(auto) {
            self.tool_.check_permissions = std::move(fn);
            return std::forward<decltype(self)>(self);
        }

        auto execute(this auto &&self, std::function<std::string(const nlohmann::json &)> fn) -> decltype(auto) {
            self.tool_.execute = std::move(fn);
            self.has_execute_ = self.tool_.execute != nullptr;
            return std::forward<decltype(self)>(self);
        }

        auto execute_rich(this auto &&self, std::function<ToolOutput(const nlohmann::json &)> fn) -> decltype(auto) {
            self.tool_.execute_rich = std::move(fn);
            self.has_execute_rich_ = self.tool_.execute_rich != nullptr;
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> std::expected<Tool, std::string> {
            if (!has_execute_ && !has_execute_rich_) {
                return std::unexpected("tool spec builder requires execute or execute_rich");
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
