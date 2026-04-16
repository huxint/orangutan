#include "bootstrap/identity.hpp"
#include "types/base.hpp"
#include "utils/format.hpp"

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orangutan::bootstrap {

    namespace {

        constexpr base::u64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
        constexpr base::u64 FNV_PRIME = 1099511628211ULL;
        constexpr std::size_t MAX_IDENTITY_LABEL_LENGTH = 48;

        base::u64 fnv1a_64(std::string_view input) {
            base::u64 hash = FNV_OFFSET_BASIS;
            for (const unsigned char ch : input) {
                hash ^= ch;
                hash *= FNV_PRIME;
            }
            return hash;
        }

        std::string sanitize_identity_component(std::string_view value) {
            std::string sanitized;
            sanitized.reserve(value.size());

            bool last_was_separator = false;
            for (const unsigned char ch : value) {
                if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.') {
                    sanitized.push_back(static_cast<char>(ch));
                    last_was_separator = false;
                    continue;
                }

                if (!last_was_separator) {
                    sanitized.push_back('_');
                    last_was_separator = true;
                }
            }

            while (!sanitized.empty() && sanitized.front() == '_') {
                sanitized.erase(sanitized.begin());
            }
            while (!sanitized.empty() && sanitized.back() == '_') {
                sanitized.pop_back();
            }

            if (sanitized.empty()) {
                sanitized = "identity";
            }
            if (sanitized.size() > MAX_IDENTITY_LABEL_LENGTH) {
                sanitized.resize(MAX_IDENTITY_LABEL_LENGTH);
            }
            return sanitized;
        }

        std::string make_identity_slug(const std::string &jid) {
            return utils::format("{}-{:016x}", sanitize_identity_component(jid), fnv1a_64(jid));
        }

        std::string normalize_path(const std::filesystem::path &path) {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (ec) {
                return path.lexically_normal().string();
            }
            return canonical.string();
        }

        std::string default_workspace_root_path() {
            const char *home = std::getenv("HOME");
            if (home == nullptr || std::string_view{home}.empty()) {
                throw std::runtime_error("HOME is not set; unable to resolve default workspace '~/.orangutan/workspace/main'");
            }
            return normalize_path(std::filesystem::path(home) / ".orangutan" / "workspace" / "main");
        }

    } // namespace

    std::string resolve_workspace_root(std::string_view workspace) {
        std::filesystem::path path = workspace.empty() ? std::filesystem::path(default_workspace_root_path()) : std::filesystem::path(workspace);
        if (!path.is_absolute()) {
            path = std::filesystem::absolute(path);
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            throw std::runtime_error("failed to inspect workspace path: " + path.string() + ": " + ec.message());
        }

        if (!exists) {
            if (!std::filesystem::create_directories(path, ec) && ec) {
                throw std::runtime_error("failed to create workspace: " + path.string() + ": " + ec.message());
            }
        }

        if (!std::filesystem::is_directory(path, ec) || ec) {
            throw std::runtime_error("workspace is not a directory: " + path.string());
        }

        return normalize_path(path);
    }

    std::filesystem::path workspace_state_root(std::string_view workspace_root) {
        if (workspace_root.empty()) {
            return {};
        }
        return std::filesystem::path(workspace_root) / ".orangutan";
    }

    std::filesystem::path workspace_skills_root(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "skills";
    }

    std::filesystem::path workspace_hooks_root(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "hooks";
    }

    std::filesystem::path workspace_memory_root(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "memory";
    }

    std::filesystem::path workspace_identity_root(std::string_view workspace_root, std::string_view runtime_key) {
        if (workspace_root.empty() || runtime_key.empty()) {
            return {};
        }
        return workspace_state_root(workspace_root) / "identities" / make_identity_slug(std::string(runtime_key));
    }

    std::filesystem::path workspace_exports_root(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "exports";
    }

    std::filesystem::path workspace_session_store_path(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "sessions.db";
    }

    std::filesystem::path workspace_memory_store_path(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "memory.db";
    }

    std::filesystem::path workspace_automation_store_path(std::string_view workspace_root) {
        return workspace_state_root(workspace_root) / "automation.db";
    }

    std::string derive_cli_runtime_key(std::string_view agent_key) {
        if (agent_key.empty() || agent_key == "default") {
            return "cli:local";
        }

        return "agent:" + std::string(agent_key) + "|cli:local";
    }

    std::string derive_cli_session_scope(std::string_view agent_key) {
        if (agent_key.empty() || agent_key == "default") {
            return {};
        }

        return "agent:" + std::string(agent_key);
    }

    RuntimeIdentity derive_cli_identity(const std::string &workspace_root, std::string_view agent_key) {
        const auto runtime_key = derive_cli_runtime_key(agent_key);
        RuntimeIdentity identity{
            .workspace = workspace_root,
            .runtime_key = runtime_key,
            .memory_scope = derive_cli_session_scope(agent_key),
        };

        return identity;
    }

    std::string derive_channel_runtime_key(std::string_view jid, std::string_view agent_key) {
        if (jid.empty()) {
            return {};
        }

        if (agent_key.empty()) {
            return "jid:" + std::string(jid);
        }

        return "agent:" + std::string(agent_key) + "|jid:" + std::string(jid);
    }

    RuntimeIdentity derive_channel_identity(const std::string &workspace_root, std::string_view jid, std::string_view agent_key) {
        const auto runtime_key = derive_channel_runtime_key(jid, agent_key);
        RuntimeIdentity identity{
            .workspace = workspace_root,
            .runtime_key = runtime_key,
            .memory_scope = runtime_key,
        };
        return identity;
    }

    RuntimeIdentity derive_child_identity(const std::string &workspace_root, std::string_view raw_caller_id, std::string_view agent_key) {
        if (raw_caller_id.empty() || raw_caller_id == "cli:local") {
            return derive_cli_identity(workspace_root, agent_key);
        }

        return derive_channel_identity(workspace_root, raw_caller_id, agent_key);
    }

    std::string append_agent_prompt_guidance(const std::string &system_prompt, const std::vector<std::string> &team_agents, bool is_child_run) {
        std::string prompt = system_prompt;

        const auto append_separator = [&prompt] {
            if (!prompt.empty()) {
                prompt += "\n\n";
            }
        };

        if (is_child_run) {
            append_separator();
            prompt += "# Worker agent mode\n";
            prompt += "You are a worker agent handling a delegated task.\n";
            prompt += "- Complete the assigned task fully — don't gold-plate, but don't leave it half-done.\n";
            prompt += "- When complete, respond with a concise report covering what was done and key findings.\n";
            prompt += "- You cannot spawn additional agents.\n";
            prompt += "- Use absolute file paths in your final response so the coordinator can navigate to them.";
            return prompt;
        }

        if (team_agents.empty()) {
            return prompt;
        }

        append_separator();
        prompt += "# Agent coordination\n";
        prompt += "The following agents are available for delegation: ";
        for (std::size_t index = 0; index < team_agents.size(); ++index) {
            if (index > 0) {
                prompt += ", ";
            }
            prompt += team_agents[index];
        }
        prompt += ".\n\n";
        prompt += "Use agent delegation for self-contained, parallelizable tasks:\n";
        prompt += "- `agent_spawn`: Start a worker agent. Returns immediately with a run_id.\n";
        prompt += "- `agent_send_message`: Send a message to a running agent.\n";
        prompt += "- `agent_stop`: Stop a running agent.\n\n";
        prompt += "Workers report results via <task-notification> messages. "
                  "Avoid spawning agents for trivial tasks you can do faster yourself.";
        return prompt;
    }

} // namespace orangutan::bootstrap
