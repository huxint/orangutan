#pragma once

#include <array>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "utils/transparent-lookup.hpp"

namespace orangutan::config::detail {

    [[nodiscard]]
    inline const nlohmann::json *find_member(const nlohmann::json &object, std::string_view key) {
        if (!object.is_object()) {
            return nullptr;
        }

        using json_object_type = nlohmann::json::object_t;
        const auto &object_ref = object.get_ref<const json_object_type &>();
        const auto it = utils::transparent_find(object_ref, key);
        return it == object_ref.end() ? nullptr : &it->second;
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

    template <typename object_type>
    using string_member_field = std::pair<std::string_view, std::string object_type::*>;

    template <typename object_type>
    [[nodiscard]]
    inline bool assign_string_member(const nlohmann::json &json_object, std::string_view key, object_type &target, std::string object_type::*member) {
        const auto *value = find_member(json_object, key);
        if (value == nullptr || !value->is_string()) {
            return false;
        }

        target.*member = value->template get<std::string>();
        return true;
    }

    template <typename object_type, std::size_t N>
    inline void assign_string_members(const nlohmann::json &json_object, object_type &target, const std::array<string_member_field<object_type>, N> &fields) {
        for (const auto &[key, member] : fields) {
            static_cast<void>(assign_string_member(json_object, key, target, member));
        }
    }

    template <typename object_type, typename number_type>
        requires((!std::same_as<number_type, bool>) && (std::integral<number_type> || std::floating_point<number_type>))
    [[nodiscard]]
    inline bool assign_number_member(const nlohmann::json &json_object, std::string_view key, object_type &target, number_type object_type::*member) {
        const auto *value = find_member(json_object, key);
        if (value == nullptr) {
            return false;
        }

        if constexpr (std::integral<number_type>) {
            if (!value->is_number_integer()) {
                return false;
            }
        } else {
            if (!value->is_number()) {
                return false;
            }
        }

        target.*member = value->template get<number_type>();
        return true;
    }

    template <typename object_type, typename number_type>
        requires((!std::same_as<number_type, bool>) && (std::integral<number_type> || std::floating_point<number_type>))
    [[nodiscard]]
    inline bool assign_optional_number_member(const nlohmann::json &json_object, std::string_view key, object_type &target, std::optional<number_type> object_type::*member) {
        const auto *value = find_member(json_object, key);
        if (value == nullptr) {
            return false;
        }

        if constexpr (std::integral<number_type>) {
            if (!value->is_number_integer()) {
                return false;
            }
        } else {
            if (!value->is_number()) {
                return false;
            }
        }

        target.*member = value->template get<number_type>();
        return true;
    }

    template <typename object_type>
    [[nodiscard]]
    inline bool assign_bool_member(const nlohmann::json &json_object, std::string_view key, object_type &target, bool object_type::*member) {
        const auto *value = find_member(json_object, key);
        if (value == nullptr || !value->is_boolean()) {
            return false;
        }

        target.*member = value->template get<bool>();
        return true;
    }

} // namespace orangutan::config::detail
