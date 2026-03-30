#include "core/tools/tool.hpp"

#include <ctre.hpp>
#include <exception>
#include <spdlog/spdlog.h>
#include <utility>

namespace orangutan {

    namespace {

        template <ctll::fixed_string Pattern>
        bool scrub_redact1(std::string &text) {
            std::string scrubbed;
            scrubbed.reserve(text.size());
            const auto input = std::string_view{text};
            const char *pos = input.data();
            const char *end = pos + input.size();
            bool changed = false;

            for (auto match : ctre::search_all<Pattern>(input)) {
                const auto full = match.template get<0>().to_view();
                scrubbed.append(pos, full.data() - pos);
                scrubbed.append(match.template get<1>().to_view());
                scrubbed += "[REDACTED]";
                pos = full.data() + full.size();
                changed = true;
            }
            if (!changed) {
                return false;
            }
            scrubbed.append(pos, static_cast<std::std::size_t>(end - pos));
            text = std::move(scrubbed);
            return true;
        }

        template <ctll::fixed_string Pattern>
        bool scrub_redact2(std::string &text) {
            std::string scrubbed;
            scrubbed.reserve(text.size());
            const auto input = std::string_view{text};
            const char *pos = input.data();
            const char *end = pos + input.size();
            bool changed = false;

            for (auto match : ctre::search_all<Pattern>(input)) {
                const auto full = match.template get<0>().to_view();
                scrubbed.append(pos, full.data() - pos);
                scrubbed.append(match.template get<1>().to_view());
                scrubbed += "[REDACTED]";
                scrubbed.append(match.template get<2>().to_view());
                pos = full.data() + full.size();
                changed = true;
            }
            if (!changed) {
                return false;
            }
            scrubbed.append(pos, static_cast<std::std::size_t>(end - pos));
            text = std::move(scrubbed);
            return true;
        }

    } // namespace

    std::string scrub_tool_output(std::string_view text) {
        std::string result{text};
        bool redacted = false;

        redacted |= scrub_redact1<R"((sk-ant-api\d{2}-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= scrub_redact1<R"((sk-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= scrub_redact1<R"((key-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= scrub_redact1<R"((Bearer\s+)[A-Za-z0-9_.\-/+=]{20,})">(result);
        redacted |= scrub_redact2<R"(([Aa]pi[_\-]?[Kk]ey\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);
        redacted |= scrub_redact2<R"(([Pp]assword\s*[:=]\s*["']?)[^\s"']{8,}(["']?))">(result);
        redacted |= scrub_redact2<R"(([Tt]oken\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);
        redacted |= scrub_redact2<R"(([Ss]ecret\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);

        if (redacted) {
            spdlog::warn("Credential scrubbing: redacted sensitive content in tool output");
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

    ToolResult ToolRegistry::execute(const ToolUse &call) const {
        if (execution_guard_ != nullptr) {
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
            result = scrub_tool_output(result);
            return {.tool_use_id = call.id, .content = result, .is_error = false};
        } catch (const std::exception &e) {
            return {.tool_use_id = call.id, .content = std::string("Error: ") + e.what(), .is_error = true};
        }
    }

} // namespace orangutan
