#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "agent/agent-loop.hpp"
#include "memory/memory-age.hpp"
#include "prompt/prompt-compiler.hpp"
#include "memory/runtime-memory.hpp"
#include "prompt/system-prompt-sections.hpp"
#include "tools/registry/tool.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

namespace orangutan::agent::detail {

    inline constexpr std::size_t MAX_MEMORY_PROMPT_BYTES = 4096;

    struct DistilledMemoryEntry {
        std::string category;
        memory_type type = memory_type::user;
        std::string key;
        base::f64 importance = 0.5;
        std::string content;
    };

    struct ParsedDistilledSession {
        std::vector<DistilledMemoryEntry> memories;
        std::optional<std::string> journal_summary;
        bool journal_parse_failed = false;
    };

    [[nodiscard]]
    inline std::string hash_key(std::string_view prefix, std::string_view value) {
        return std::string(prefix) + std::to_string(std::hash<std::string_view>{}(value));
    }

    [[nodiscard]]
    inline std::optional<DistilledMemoryEntry> parse_memory_line(std::string line) {
        if (line.starts_with("memory|")) {
            line = line.substr(7);
        }

        std::vector<std::string_view> fields;
        std::string_view remaining = line;
        while (true) {
            const auto separator = remaining.find('|');
            if (separator == std::string_view::npos) {
                fields.push_back(remaining);
                break;
            }
            fields.push_back(remaining.substr(0, separator));
            remaining = remaining.substr(separator + 1);
        }

        if (fields.size() < 4) {
            return std::nullopt;
        }

        std::string_view type_text;
        std::string_view category_text;
        std::string_view key_text;
        std::string_view importance_text;
        std::string content;

        if (fields.size() == 4) {
            category_text = fields[0];
            key_text = fields[1];
            importance_text = fields[2];
            content = std::string(utils::trim_copy(fields[3]));
        } else {
            type_text = fields[0];
            category_text = fields[1];
            key_text = fields[2];
            importance_text = fields[3];
            const auto content_offset = static_cast<std::size_t>(fields[4].data() - line.data());
            content = std::string(utils::trim_copy(line.substr(content_offset)));
        }

        auto category = std::string(utils::trim_copy(category_text));
        auto key = std::string(utils::trim_copy(key_text));
        const auto trimmed_importance = utils::trim_copy(importance_text);

        if (content.empty()) {
            return std::nullopt;
        }
        if (category.empty()) {
            category = "general";
        }

        base::f64 importance = 0.5;
        std::from_chars(trimmed_importance.begin(), trimmed_importance.end(), importance);
        importance = std::clamp(importance, 0.0, 1.0);

        if (key.empty()) {
            key = hash_key("distilled.", content);
        }

        const auto type = type_text.empty() ? infer_memory_type(category) : magic_enum::enum_cast<memory_type>(type_text, magic_enum::case_insensitive).value_or(memory_type::user);

        return DistilledMemoryEntry{
            .category = std::move(category),
            .type = type,
            .key = std::move(key),
            .importance = importance,
            .content = std::move(content),
        };
    }

    [[nodiscard]]
    inline ParsedDistilledSession parse_distilled_session(const std::string &text) {
        ParsedDistilledSession parsed;
        std::stringstream stream(text);
        std::string line;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            line = utils::trim_copy(line);
            if (line.empty()) {
                continue;
            }
            if (line.starts_with("- ")) {
                line = utils::trim_copy(line.substr(2));
            }

            if (line.starts_with("journal|")) {
                const auto summary = utils::trim_copy(line.substr(std::string{"journal|"}.size()));
                if (summary.empty()) {
                    parsed.journal_parse_failed = true;
                } else {
                    parsed.journal_summary = summary;
                }
                continue;
            }
            if (line == "journal") {
                parsed.journal_parse_failed = true;
                continue;
            }
            if (!line.starts_with("memory|")) {
                continue;
            }

            if (auto memory = parse_memory_line(std::move(line)); memory.has_value()) {
                parsed.memories.push_back(std::move(*memory));
            }
        }

        constexpr std::size_t MAX_DISTILLED_MEMORIES = 8;
        if (parsed.memories.size() > MAX_DISTILLED_MEMORIES) {
            parsed.memories.resize(MAX_DISTILLED_MEMORIES);
        }
        return parsed;
    }

    [[nodiscard]]
    inline std::string build_session_memory_transcript(const std::vector<Message> &history) {
        std::string transcript;

        for (const auto &message : history) {
            utils::format_to(transcript, "{}:\n", magic_enum::enum_name(message.role()));
            for (const auto &block : message) {
                if (const auto *text = std::get_if<Text>(&block)) {
                    if (!text->text.empty()) {
                        transcript.append(text->text);
                        transcript.push_back('\n');
                    }
                    continue;
                }

                if (const auto *tool = std::get_if<ToolUse>(&block)) {
                    utils::format_to(transcript, "[tool_use] {}\n", tool->name);
                    continue;
                }

                const auto *result = std::get_if<ToolResult>(&block);
                if (result != nullptr && !result->content.empty()) {
                    transcript.append("[tool_result] ");
                    transcript.append(result->content);
                    transcript.push_back('\n');
                }
            }
            transcript.push_back('\n');
        }

        constexpr std::size_t MAX_SESSION_TRANSCRIPT_CHARS = 6000;
        if (transcript.size() <= MAX_SESSION_TRANSCRIPT_CHARS) {
            return transcript;
        }

        constexpr std::size_t RETAINED_SIDE_CHARS = 2800;
        return transcript.substr(0, RETAINED_SIDE_CHARS) + "\n...\n" + transcript.substr(transcript.size() - RETAINED_SIDE_CHARS);
    }

    [[nodiscard]]
    inline AgentLoop::SessionMemoryDistillationResult distill_session_memory(ProviderSystem &provider, const ProviderRoute &route, RuntimeMemory *memory,
                                                                             const std::vector<Message> &history) {
        AgentLoop::SessionMemoryDistillationResult result{
            .distilled = false,
            .memories_stored = 0,
            .journal_stored = false,
            .status = "No session memory distilled.",
        };

        if (memory == nullptr) {
            result.status = "Long-term memory is disabled.";
            return result;
        }

        if (history.size() < 2) {
            result.status = "Not enough session history to distill.";
            return result;
        }

        const auto transcript = build_session_memory_transcript(history);
        if (transcript.empty()) {
            result.status = "Session transcript is empty.";
            return result;
        }

        constexpr std::string_view DISTILLATION_PROMPT = "You are distilling long-term memory from a completed conversation. "
                                                         "Extract only durable, reusable information that should help future sessions. "
                                                         "Prefer stable facts, preferences, project context, decisions, and lessons learned. "
                                                         "Ignore greetings, temporary chatter, and one-off execution details. "
                                                         "Return at most 9 lines. Each line must use exactly one of these formats:\n"
                                                         "memory|type|category|key|importance|content\n"
                                                         "journal|summary\n"
                                                         "- type: one of user (role/preferences/knowledge), feedback (corrections/approaches), "
                                                         "project (work/decisions/deadlines), reference (external pointers/docs)\n"
                                                         "- category: one of profile, preference, project, decision, learning, fact, task, general\n"
                                                         "- key: lowercase stable identifier like project.current or decision.agent-routing\n"
                                                         "- importance: decimal between 0 and 1\n"
                                                         "- content: concise memory text\n"
                                                         "- summary: one short session summary line, optional, at most once\n"
                                                         "Return only lines in those formats, with no extra commentary.";

        try {
            std::vector<Message> messages;
            messages.push_back(Message::user().text(transcript));
            std::vector<ToolDef> no_tools;
            const auto response = provider.route(route).system(DISTILLATION_PROMPT).messages(messages).tools(no_tools).max_tokens(1024).send_blocking().response;

            std::string distilled_text;
            for (const auto &block : response.content) {
                if (const auto *text = std::get_if<Text>(&block)) {
                    distilled_text += text->text;
                }
            }

            const auto parsed = parse_distilled_session(distilled_text);
            for (const auto &memory_entry : parsed.memories) {
                memory->update(memory_entry.key, memory_entry.content, memory_entry.category, memory_entry.type, true, "session:distilled", memory_entry.importance);
            }

            result.distilled = !parsed.memories.empty();
            result.memories_stored = parsed.memories.size();
            if (parsed.journal_summary.has_value()) {
                const auto journal_result = memory->store_journal_summary(*parsed.journal_summary);
                result.journal_stored = journal_result.stored;
            }

            if (result.distilled || result.journal_stored) {
                result.status = "Session distilled into long-term memory.";
            } else {
                result.status = "Session distillation produced no durable memories.";
            }
            if (parsed.journal_parse_failed && !result.journal_stored) {
                result.status += " journaling was skipped.";
            }
        } catch (const std::exception &e) {
            spdlog::warn("Session memory distillation failed: {}", e.what());
            result.status = std::string("Session distillation failed: ") + e.what();
        }

        return result;
    }

    [[nodiscard]]
    inline std::string build_system_prompt(const prompt::EnvironmentInfo &env_info, std::string_view user_input, std::string_view skills_prompt, const ToolRegistry &tools,
                                           RuntimeMemory *memory) {
        std::vector<prompt::PromptSection> sections;
        sections.push_back(prompt::PromptSection{
            .id = "system.default",
            .kind = prompt::prompt_section_kind::static_section,
            .classification = prompt::prompt_section_classification::must_keep,
            .priority = 100,
            .content = prompt::build_default_system_prompt(env_info),
            .cache_key_part = "system.default",
        });
        if (!skills_prompt.empty()) {
            sections.push_back(prompt::PromptSection{
                .id = "system.skills",
                .kind = prompt::prompt_section_kind::dynamic_section,
                .classification = prompt::prompt_section_classification::important,
                .priority = 90,
                .content = std::string(skills_prompt),
                .cache_key_part = "system.skills",
            });
        }

        const auto deferred = tools.deferred_tool_summaries();
        if (!deferred.empty()) {
            std::string deferred_section = "<available-deferred-tools>\n";
            deferred_section += "The following tools are available but not yet loaded. Use the `tool_search` tool to discover and enable them before use.\n";
            for (const auto &tool : deferred) {
                deferred_section += tool.name;
                deferred_section.push_back('\n');
            }
            deferred_section += "</available-deferred-tools>";

            sections.push_back(prompt::PromptSection{
                .id = "system.deferred_tools",
                .kind = prompt::prompt_section_kind::dynamic_section,
                .classification = prompt::prompt_section_classification::important,
                .priority = 80,
                .content = std::move(deferred_section),
                .cache_key_part = "system.deferred_tools",
            });
        }

        auto prompt = prompt::compile_prompt(prompt::PromptBuildInput{
                                                 .sections = std::move(sections),
                                                 .token_budget = 0,
                                             })
                          .full_prompt;

        if (memory == nullptr) {
            return prompt;
        }

        const auto records = memory->prompt_memories(user_input, 8);
        if (records.empty()) {
            return prompt;
        }

        std::string memory_block = "\n\n<relevant-memories>\n";
        memory_block.append("Historical notes for context. Memories older than 1 day should be verified before acting on them.\n");

        std::size_t used = std::string{"<relevant-memories>\n</relevant-memories>"}.size();
        bool wrote_any = false;

        for (const auto &record : records) {
            std::string candidate;
            utils::format_to(candidate, "- [{}:{}] {}", magic_enum::enum_name(record.type), record.key, record.content);
            const auto caveat = memory_freshness_caveat(record.updated_at);
            if (!caveat.empty()) {
                candidate.push_back(' ');
                candidate.append(caveat);
            }
            if (used + candidate.size() + 1 > MAX_MEMORY_PROMPT_BYTES) {
                if (wrote_any) {
                    break;
                }

                const auto remaining = MAX_MEMORY_PROMPT_BYTES > used + 4 ? MAX_MEMORY_PROMPT_BYTES - used - 4 : 0;
                candidate = remaining == 0 ? "..." : candidate.substr(0, remaining) + "...";
            }

            memory_block.append(candidate);
            memory_block.push_back('\n');
            used += candidate.size() + 1;
            wrote_any = true;
        }

        if (!wrote_any) {
            return prompt;
        }

        memory_block.append("</relevant-memories>");
        return prompt + memory_block;
    }

} // namespace orangutan::agent::detail
