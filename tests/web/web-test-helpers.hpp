#pragma once

#include "config/config.hpp"
#include "storage/session-store.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace orangutan::testing::web {

    inline ProfileConfig make_profile(std::initializer_list<std::pair<const std::string, ModelConfig>> models, std::string api_key = "test-key",
                                      std::string base_url = "https://example.test") {
        ProfileConfig profile{
            .base_url = std::move(base_url),
            .api_key = std::move(api_key),
        };
        for (const auto &[name, model] : models) {
            profile.models.emplace(name, model);
        }
        return profile;
    }

    inline SessionMetadata make_session_metadata(std::string model, std::string scope_key, std::string agent_key, std::string origin_kind, std::string origin_ref) {
        return SessionMetadata{
            .model = std::move(model),
            .scope_key = std::move(scope_key),
            .agent_key = std::move(agent_key),
            .origin_kind = std::move(origin_kind),
            .origin_ref = std::move(origin_ref),
        };
    }

} // namespace orangutan::testing::web
