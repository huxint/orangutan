#include "app/runtime/identity.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orangutan {

namespace {

constexpr uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr uint64_t fnv_prime = 1099511628211ULL;
constexpr size_t max_identity_label_length = 48;

uint64_t fnv1a_64(std::string_view input) {
    uint64_t hash = fnv_offset_basis;
    for (const unsigned char ch : input) {
        hash ^= ch;
        hash *= fnv_prime;
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
    if (sanitized.size() > max_identity_label_length) {
        sanitized.resize(max_identity_label_length);
    }
    return sanitized;
}

std::string make_identity_slug(const std::string &jid) {
    std::ostringstream out;
    out << sanitize_identity_component(jid) << '-' << std::hex << std::setw(16) << std::setfill('0') << fnv1a_64(jid);
    return out.str();
}

std::string normalize_path(const std::filesystem::path &path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return path.lexically_normal().string();
    }
    return canonical.string();
}

} // namespace

std::string resolve_workspace_root(const std::string &workspace) {
    if (workspace.empty()) {
        return {};
    }

    std::filesystem::path path(workspace);
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

std::string derive_cli_runtime_key(const std::string &agent_key) {
    if (agent_key.empty() || agent_key == "default") {
        return "cli:local";
    }

    return "agent:" + agent_key + "|cli:local";
}

std::string derive_cli_session_scope(const std::string &agent_key) {
    if (agent_key.empty() || agent_key == "default") {
        return {};
    }

    return "agent:" + agent_key;
}

RuntimeIdentity derive_cli_identity(const std::string &workspace_root, const std::string &agent_key) {
    const auto runtime_key = derive_cli_runtime_key(agent_key);
    RuntimeIdentity identity{
        .workspace = workspace_root,
        .runtime_key = runtime_key,
        .memory_scope = derive_cli_session_scope(agent_key),
    };

    return identity;
}

std::string derive_channel_runtime_key(const std::string &jid, const std::string &agent_key) {
    if (jid.empty()) {
        return {};
    }

    if (agent_key.empty()) {
        return "jid:" + jid;
    }

    return "agent:" + agent_key + "|jid:" + jid;
}

RuntimeIdentity derive_channel_identity(const std::string &workspace_root, const std::string &jid, const std::string &agent_key) {
    const auto runtime_key = derive_channel_runtime_key(jid, agent_key);
    RuntimeIdentity identity{
        .workspace = {},
        .runtime_key = runtime_key,
        .memory_scope = runtime_key,
    };

    if (workspace_root.empty()) {
        return identity;
    }

    auto identity_path = std::filesystem::path(workspace_root) / make_identity_slug(runtime_key);
    std::error_code ec;
    if (!std::filesystem::create_directories(identity_path, ec) && ec) {
        throw std::runtime_error("failed to create identity workspace: " + identity_path.string() + ": " + ec.message());
    }

    identity.workspace = normalize_path(identity_path);
    return identity;
}

RuntimeIdentity derive_child_identity(const std::string &workspace_root, const std::string &raw_caller_id, const std::string &agent_key) {
    if (raw_caller_id.empty() || raw_caller_id == "cli:local") {
        return derive_cli_identity(workspace_root, agent_key);
    }

    return derive_channel_identity(workspace_root, raw_caller_id, agent_key);
}

std::string append_subagent_prompt_guidance(const std::string &system_prompt, const std::vector<std::string> &allowed_child_agents, bool is_child_run) {
    std::ostringstream prompt;
    prompt << system_prompt;

    const auto append_separator = [&prompt] {
        if (!prompt.str().empty()) {
            prompt << "\n\n";
        }
    };

    if (is_child_run) {
        append_separator();
        prompt << "Delegated worker mode:\n"
               << "- You are a delegated worker handling a task from another Orangutan runtime.\n"
               << "- Complete the assigned task and return a concise result.\n"
               << "- You cannot spawn subagents.";
        return prompt.str();
    }

    if (allowed_child_agents.empty()) {
        return prompt.str();
    }

    append_separator();
    prompt << "Subagent delegation is available for: ";
    for (size_t index = 0; index < allowed_child_agents.size(); ++index) {
        if (index > 0) {
            prompt << ", ";
        }
        prompt << allowed_child_agents[index];
    }
    prompt << ".\n"
           << "- Use `subagent_spawn` when a self-contained task can be delegated to one of those child agents.\n"
           << "- Use `subagent_status` to check whether a child run has finished without blocking.\n"
           << "- Use `subagent_wait` when you need the child result before continuing.\n"
           << "- If you can continue making progress without the child result yet, keep working and poll later with `subagent_status`.";
    return prompt.str();
}

} // namespace orangutan
