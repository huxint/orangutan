#include "app/runtime/memory-context.hpp"

namespace orangutan {
    namespace {

        std::filesystem::path resolve_workspace_path(const std::string &workspace, const std::string &configured) {
            if (workspace.empty() || configured.empty()) {
                return {};
            }

            auto configured_path = std::filesystem::path(configured);
            if (configured_path.is_absolute()) {
                return configured_path;
            }

            return std::filesystem::path(workspace) / configured_path;
        }

    } // namespace

    std::filesystem::path RuntimeMemoryContext::snapshot_path() const {
        if (!mirror_enabled()) {
            return {};
        }

        return resolve_workspace_path(workspace, mirror.mirror_file);
    }

    std::filesystem::path RuntimeMemoryContext::journal_dir() const {
        if (!mirror_enabled()) {
            return {};
        }

        return resolve_workspace_path(workspace, mirror.journal_dir);
    }

    RuntimeMemoryContext make_runtime_memory_context(const RuntimeIdentity &identity, const Config::MemoryConfig &memory_cfg) {
        return {
            .scope = identity.memory_scope,
            .workspace = identity.workspace,
            .mirror =
                {
                    .enabled = memory_cfg.mirror_enabled,
                    .mirror_file = memory_cfg.mirror_file,
                    .journal_dir = memory_cfg.journal_dir,
                },
        };
    }

} // namespace orangutan
