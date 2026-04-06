#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace orangutan::config::detail {

    [[nodiscard]]
    inline const nlohmann::json *find_member(const nlohmann::json &object, std::string_view key) {
        if (!object.is_object()) {
            return nullptr;
        }

        const auto it = object.find(std::string{key});
        return it == object.end() ? nullptr : &*it;
    }

    [[nodiscard]]
    inline const nlohmann::json *find_object_member(const nlohmann::json &object, std::string_view key) {
        const auto *value = find_member(object, key);
        return value != nullptr && value->is_object() ? value : nullptr;
    }

    [[nodiscard]]
    inline const nlohmann::json *find_array_member(const nlohmann::json &object, std::string_view key) {
        const auto *value = find_member(object, key);
        return value != nullptr && value->is_array() ? value : nullptr;
    }

} // namespace orangutan::config::detail
