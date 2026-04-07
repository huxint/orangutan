#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace orangutan::tools::schema_fragments {

    [[nodiscard]]
    inline nlohmann::json empty_object_schema() {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        };
    }

    [[nodiscard]]
    inline nlohmann::json empty_object() {
        return empty_object_schema();
    }

    [[nodiscard]]
    inline nlohmann::json empty_object_fragment() {
        return empty_object_schema();
    }

    [[nodiscard]]
    inline nlohmann::json id_field() {
        return {
            {"type", "string"},
        };
    }

    [[nodiscard]]
    inline nlohmann::json op_enum(std::initializer_list<std::string_view> values) {
        auto ops = nlohmann::json::array();
        for (const auto value : values) {
            ops.push_back(value);
        }

        return {
            {"type", "string"},
            {"enum", std::move(ops)},
        };
    }

    [[nodiscard]]
    inline nlohmann::json delivery_mode_field() {
        return op_enum({"silent", "notify"});
    }

    [[nodiscard]]
    inline nlohmann::json delivery_targets_field() {
        return {
            {"type", "array"},
            {"items", {{"type", "string"}}},
        };
    }

    [[nodiscard]]
    inline nlohmann::json object_with_required(nlohmann::json properties, std::initializer_list<std::string_view> required_fields) {
        auto required = nlohmann::json::array();
        for (const auto field : required_fields) {
            required.push_back(field);
        }

        return {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
        };
    }

    [[nodiscard]]
    inline nlohmann::json delivery_fields() {
        return {
            {"channel", {{"type", "string"}}},
            {"thread_id", {{"type", "string"}}},
        };
    }

    [[nodiscard]]
    inline nlohmann::json op_id_object_schema() {
        return object_with_required(
            {
                {"op", {{"type", "string"}}},
                {"id", id_field()},
            },
            {"op", "id"});
    }

} // namespace orangutan::tools::schema_fragments
