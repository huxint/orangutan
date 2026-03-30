#include "core/tools/tool.hpp"

#include <ctre.hpp>
#include <exception>
#include <spdlog/spdlog.h>
#include <utility>

namespace orangutan {

    namespace {
        constexpr std::string_view redaction_marker = "[REDACTED]";

        template <ctll::fixed_string Pattern, class Rewriter>
        bool rewrite_matches(std::string &text, Rewriter rewriter) {
            const std::string_view source{text};
            auto matches = ctre::search_all<Pattern>(source);
            auto it = matches.begin();

            if (it == matches.end()) {
                return false;
            }

            std::string result;
            result.reserve(source.size());

            std::string_view::const_iterator current = source.begin();

            for (; it != matches.end(); ++it) {
                const auto whole = (*it).template get<0>().to_view();
                result.append(current, whole.begin());
                rewriter(result, *it);
                current = whole.end();
            }

            result.append(current, source.end());
            text = std::move(result);
            return true;
        }

        template <ctll::fixed_string Pattern>
        bool redact_after_prefix(std::string &text) {
            return rewrite_matches<Pattern>(text, [](std::string &out, const auto &match) {
                out.append(match.template get<1>().to_view());
                out.append(redaction_marker);
            });
        }

        template <ctll::fixed_string Pattern>
        bool redact_between_edges(std::string &text) {
            return rewrite_matches<Pattern>(text, [](std::string &out, const auto &match) {
                out.append(match.template get<1>().to_view());
                out.append(redaction_marker);
                out.append(match.template get<2>().to_view());
            });
        }

    } // namespace

    std::string scrub_tool_output(std::string_view text) {
        std::string result{text};
        bool redacted = false;

        redacted |= redact_after_prefix<R"((sk-ant-api\d{2}-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= redact_after_prefix<R"((sk-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= redact_after_prefix<R"((key-)[A-Za-z0-9_\-]{20,})">(result);
        redacted |= redact_after_prefix<R"((Bearer\s+)[A-Za-z0-9_.\-/+=]{20,})">(result);
        redacted |= redact_between_edges<R"(([Aa]pi[_\-]?[Kk]ey\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);
        redacted |= redact_between_edges<R"(([Pp]assword\s*[:=]\s*["']?)[^\s"']{8,}(["']?))">(result);
        redacted |= redact_between_edges<R"(([Tt]oken\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);
        redacted |= redact_between_edges<R"(([Ss]ecret\s*[:=]\s*["']?)[A-Za-z0-9_.\-/+=]{16,}(["']?))">(result);

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
            return {call.id, "Error: unknown tool '" + call.name + "'", true};
        }

        try {
            std::string result = it->second.execute(call.input);
            result = scrub_tool_output(result);
            return {call.id, result, false};
        } catch (const std::exception &e) {
            return {call.id, std::string("Error: ") + e.what(), true};
        }
    }

} // namespace orangutan
