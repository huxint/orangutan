#pragma once

#include "infra/config/config.hpp"
#include "app/runtime/identity.hpp"

#include <filesystem>
#include <string>

namespace orangutan {

    struct MemoryMirrorSettings {
        bool enabled = false;
        std::string mirror_file = "MEMORY.md";
        std::string journal_dir = "memory";
    };

    struct RuntimeMemoryContext {
        std::string scope;
        std::string workspace;
        MemoryMirrorSettings mirror;

        [[nodiscard]]
        bool has_workspace() const {
            return !workspace.empty();
        }

        [[nodiscard]]
        bool mirror_enabled() const {
            return mirror.enabled && has_workspace();
        }

        [[nodiscard]]
        std::filesystem::path snapshot_path() const;

        [[nodiscard]]
        std::filesystem::path journal_dir() const;
    };

    [[nodiscard]]
    RuntimeMemoryContext make_runtime_memory_context(const RuntimeIdentity &identity, const Config::MemoryConfig &memory_cfg);

} // namespace orangutan
