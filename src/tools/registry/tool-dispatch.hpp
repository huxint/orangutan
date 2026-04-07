#pragma once

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

        ToolDispatch &op_field(std::string field_name) {
            op_field_ = std::move(field_name);
            return *this;
        }

        ToolDispatch &unknown_op_error(std::string message) {
            unknown_op_error_ = std::move(message);
            return *this;
        }

        ToolDispatch &unknown_op_error_formatter(unknown_op_error_formatter_fn formatter) {
            unknown_op_error_formatter_ = std::move(formatter);
            return *this;
        }

        ToolDispatch &missing_op_error_formatter(missing_op_error_formatter_fn formatter) {
            missing_op_error_formatter_ = std::move(formatter);
            return *this;
        }

        ToolDispatch &on(std::string op, Handler fn) {
            handlers_.insert_or_assign(std::move(op), std::move(fn));
            return *this;
        }

        [[nodiscard]]
        Response run(const nlohmann::json &input) const {
            if (!input.contains(op_field_)) {
                return {.message = missing_op_error_formatter_(op_field_), .is_error = true};
            }

            if (!input.at(op_field_).is_string()) {
                return {.message = "invalid type for field: " + op_field_, .is_error = true};
            }

            const auto op = input.at(op_field_).get<std::string>();
            const auto it = handlers_.find(op);
            if (it == handlers_.end()) {
                const auto message = unknown_op_error_formatter_ != nullptr ? unknown_op_error_formatter_(op) : unknown_op_error_;
                return {.message = message, .is_error = true};
            }

            return it->second(input);
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
