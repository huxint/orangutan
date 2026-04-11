#include "tool-registry.hpp"

#include "utils/utf8.hpp"

#include <algorithm>
#include <ctre.hpp>
#include <exception>
#include <spdlog/spdlog.h>
#include <utility>

namespace orangutan::tools {

    namespace {
        constexpr std::string_view REDACTION_MARKER = "[REDACTED]";

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
                out.append(REDACTION_MARKER);
            });
        }

        template <ctll::fixed_string Pattern>
        bool redact_between_edges(std::string &text) {
            return rewrite_matches<Pattern>(text, [](std::string &out, const auto &match) {
                out.append(match.template get<1>().to_view());
                out.append(REDACTION_MARKER);
                out.append(match.template get<2>().to_view());
            });
        }

    } // namespace

    std::string scrub_tool_output(std::string_view text) {
        std::string result = utf8::sanitize(text);
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
            if (definition_filter_ && !definition_filter_(tool)) {
                continue;
            }
            if (tool.deferred && !discovered_tools_.contains(tool.definition.name)) {
                continue;
            }
            defs.push_back(tool.definition);
        }
        return defs;
    }

    void ToolRegistry::discover_tool(const std::string &name) const {
        discovered_tools_.insert(name);
    }

    void ToolRegistry::discover_deferred_tools() const {
        for (const auto &[name, tool] : tools_) {
            if (!tool.deferred) {
                continue;
            }
            if (definition_filter_ && !definition_filter_(tool)) {
                continue;
            }
            discovered_tools_.insert(name);
        }
    }

    void ToolRegistry::clear_discovered() const {
        discovered_tools_.clear();
    }

    bool ToolRegistry::has_deferred_tools() const {
        return std::ranges::any_of(tools_, [this](const auto &entry) {
            const auto &tool = entry.second;
            return tool.deferred && (!definition_filter_ || definition_filter_(tool));
        });
    }

    std::vector<DeferredToolSummary> ToolRegistry::deferred_tool_summaries() const {
        std::vector<DeferredToolSummary> summaries;
        for (const auto &[_, tool] : tools_) {
            if (!tool.deferred) {
                continue;
            }
            if (definition_filter_ && !definition_filter_(tool)) {
                continue;
            }
            if (discovered_tools_.contains(tool.definition.name)) {
                continue;
            }
            summaries.push_back({.name = tool.definition.name, .description = tool.definition.description});
        }
        return summaries;
    }

    const ToolDef *ToolRegistry::find_definition(std::string_view name) const {
        auto it = tools_.find(std::string{name});
        if (it == tools_.end()) {
            return nullptr;
        }
        if (definition_filter_ && !definition_filter_(it->second)) {
            return nullptr;
        }
        return &it->second.definition;
    }

    const Tool *ToolRegistry::find_tool(std::string_view name) const {
        auto it = tools_.find(std::string{name});
        if (it == tools_.end()) {
            return nullptr;
        }
        if (definition_filter_ && !definition_filter_(it->second)) {
            return nullptr;
        }
        return &it->second;
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
            if (it->second.execute_rich) {
                auto output = it->second.execute_rich(call.input);
                output.text = scrub_tool_output(output.text);
                ToolResult result;
                result.tool_use_id = call.id;
                result.content = std::move(output.text);
                result.images = std::move(output.images);
                return result;
            }
            std::string result = it->second.execute(call.input);
            result = scrub_tool_output(result);
            return {call.id, result, false};
        } catch (const std::exception &e) {
            return {call.id, std::string("Error: ") + e.what(), true};
        }
    }

} // namespace orangutan::tools
