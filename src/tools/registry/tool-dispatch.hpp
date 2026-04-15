#pragma once

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace orangutan::tools {

    class ToolDispatch {
    public:
        struct Response {
            std::string message;
            bool is_error = false;
        };

        using response = Response;

        using Handler = std::function<Response(const nlohmann::json &)>;
        using handler = Handler;
        using missing_op_error_formatter_fn = std::function<std::string(std::string_view)>;
        using unknown_op_error_formatter_fn = std::function<std::string(std::string_view)>;

        auto op_field(this auto &&self, std::string field_name) -> decltype(auto) {
            self.op_field_ = std::move(field_name);
            return std::forward<decltype(self)>(self);
        }

        auto unknown_op_error(this auto &&self, std::string message) -> decltype(auto) {
            self.unknown_op_error_ = std::move(message);
            return std::forward<decltype(self)>(self);
        }

        auto unknown_op_error_formatter(this auto &&self, unknown_op_error_formatter_fn formatter) -> decltype(auto) {
            self.unknown_op_error_formatter_ = std::move(formatter);
            return std::forward<decltype(self)>(self);
        }

        auto missing_op_error_formatter(this auto &&self, missing_op_error_formatter_fn formatter) -> decltype(auto) {
            self.missing_op_error_formatter_ = std::move(formatter);
            return std::forward<decltype(self)>(self);
        }

        auto on(this auto &&self, std::string op, Handler fn) -> decltype(auto) {
            self.handlers_.insert_or_assign(std::move(op), std::move(fn));
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto run(const nlohmann::json &input) const -> std::expected<Response, std::string> {
            if (!input.contains(op_field_)) {
                return std::unexpected(missing_op_error_formatter_ != nullptr ? missing_op_error_formatter_(op_field_) : "missing required field: " + op_field_);
            }

            if (!input.at(op_field_).is_string()) {
                return std::unexpected("invalid type for field: " + op_field_);
            }

            const auto op = input.at(op_field_).get<std::string>();
            if (!handlers_.contains(op)) {
                return std::unexpected(unknown_op_error_formatter_ != nullptr ? unknown_op_error_formatter_(op) : unknown_op_error_);
            }

            return handlers_.at(op)(input);
        }

    private:
        std::string op_field_ = "op";
        std::string unknown_op_error_ = "unknown operation";
        missing_op_error_formatter_fn missing_op_error_formatter_ = [](std::string_view field_name) {
            return "missing required field: " + std::string(field_name);
        };
        unknown_op_error_formatter_fn unknown_op_error_formatter_;
        std::unordered_map<std::string, Handler> handlers_;
    };

    using tool_dispatch = ToolDispatch;

} // namespace orangutan::tools
