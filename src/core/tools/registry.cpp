#include "core/tools/tool.hpp"

#include <exception>
#include <regex>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace orangutan {

static std::string scrub_credentials(const std::string &text) {
    static const std::vector<std::pair<std::regex, std::string>> patterns = {
        {std::regex(R"((sk-ant-api\d{2}-)[A-Za-z0-9_-]{20,})"), "$1[REDACTED]"},
        {std::regex(R"((sk-)[A-Za-z0-9_-]{20,})"), "$1[REDACTED]"},
        {std::regex(R"((key-)[A-Za-z0-9_-]{20,})"), "$1[REDACTED]"},
        {std::regex(R"((Bearer\s+)[A-Za-z0-9_.\-/+=]{20,})"), "$1[REDACTED]"},
        {std::regex(R"(([Aa]pi[_-]?[Kk]ey\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))"), "$1[REDACTED]$2"},
        {std::regex(R"(([Pp]assword\s*[:=]\s*["']?)[^\s"']{8,}(["']?))"), "$1[REDACTED]$2"},
        {std::regex(R"(([Tt]oken\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))"), "$1[REDACTED]$2"},
        {std::regex(R"(([Ss]ecret\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))"), "$1[REDACTED]$2"},
    };

    std::string result = text;
    for (const auto &[pattern, replacement] : patterns) {
        auto scrubbed = std::regex_replace(result, pattern, replacement);
        if (scrubbed != result) {
            spdlog::warn("Credential scrubbing: redacted sensitive content in tool output");
            result = std::move(scrubbed);
        }
    }
    return result;
}

void ToolRegistry::register_tool(Tool tool) {
    std::string name = tool.definition.name;
    tools_.insert_or_assign(std::move(name), std::move(tool));
}

void ToolRegistry::set_execution_guard(ExecutionGuard guard) {
    execution_guard_ = std::move(guard);
}

void ToolRegistry::set_definition_filter(DefinitionFilter filter) {
    definition_filter_ = std::move(filter);
}

std::vector<ToolDef> ToolRegistry::definitions() const {
    std::vector<ToolDef> defs;
    defs.reserve(tools_.size());
    for (const auto &[_, tool] : tools_) {
        if (definition_filter_ && !definition_filter_(tool.definition)) {
            continue;
        }
        defs.push_back(tool.definition);
    }
    return defs;
}

ToolResultBlock ToolRegistry::execute(const ToolUseBlock &call) const {
    if (execution_guard_) {
        if (auto blocked = execution_guard_(call); blocked.has_value()) {
            return *blocked;
        }
    }

    auto it = tools_.find(call.name);
    if (it == tools_.end()) {
        return {.tool_use_id = call.id, .content = "Error: unknown tool '" + call.name + "'", .is_error = true};
    }

    try {
        std::string result = it->second.execute(call.input);
        result = scrub_credentials(result);
        return {.tool_use_id = call.id, .content = result, .is_error = false};
    } catch (const std::exception &e) {
        return {.tool_use_id = call.id, .content = std::string("Error: ") + e.what(), .is_error = true};
    }
}

} // namespace orangutan
