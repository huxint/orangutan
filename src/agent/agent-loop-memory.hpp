#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "agent/agent-loop.hpp"
#include "prompt/prompt-compiler.hpp"
#include "memory/runtime-memory.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

namespace orangutan::agent::detail {

    inline constexpr std::size_t MAX_MEMORY_PROMPT_BYTES = 4096;

    struct DistilledMemoryEntry {
        memory_type kind = memory_type::user;
        std::string key;
        std::string content;
    };

    struct ParsedDistilledSession {
        std::vector<DistilledMemoryEntry> memories;
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

        if (fields.size() < 3) {
            return std::nullopt;
        }

        const auto kind_text = utils::trim_copy(fields[0]);
        auto key = std::string(utils::trim_copy(fields[1]));
        const auto content_offset = static_cast<std::size_t>(fields[2].data() - line.data());
        auto content = std::string(utils::trim_copy(line.substr(content_offset)));
        if (content.empty()) {
            return std::nullopt;
        }
        if (key.empty()) {
            key = hash_key("memory.", content);
        }

        return DistilledMemoryEntry{
            .kind = magic_enum::enum_cast<memory_type>(kind_text, magic_enum::case_insensitive).value_or(memory_type::user),
            .key = std::move(key),
            .content = std::move(content),
        };
    }

    [[nodiscard]]
    inline ParsedDistilledSession parse_distilled_session(const std::string &text) {
        ParsedDistilledSession parsed;
        for (auto raw_line : utils::split_lines(text)) {
            if (!raw_line.empty() && raw_line.back() == '\r') {
                raw_line.pop_back();
            }
            auto line = std::string(utils::trim_copy(raw_line));
            if (line.starts_with("- ")) {
                line = std::string(utils::trim_copy(std::string_view{line}.substr(2)));
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

        constexpr std::string_view DISTILLATION_PROMPT =
            "You extract long-term memory from a completed conversation. Keep only details that will make future help feel more personal, accurate, and less repetitive. "
            "Remember stable user preferences, durable project context, explicit corrections, validated decisions, and useful references. "
            "Do not remember greetings, temporary command output, one-off file paths, speculation, or routine progress chatter. "
            "Write at most 8 lines. Each line must be exactly:\n"
            "memory|kind|key|content\n"
            "kind is one of user, feedback, project, reference. "
            "key is a short lowercase identifier such as preference.reply-style, project.current, feedback.testing, or reference.docs. "
            "content is one concise sentence, phrased as quiet background context for a future assistant. "
            "Return only memory lines. If there is nothing durable, return nothing.";

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
            for (const auto &entry : parsed.memories) {
                memory->remember(entry.key, entry.content, entry.kind);
            }

            result.distilled = !parsed.memories.empty();
            result.memories_stored = parsed.memories.size();
            result.status = result.distilled ? "Session distilled into long-term memory." : "Session distillation produced no durable memories.";
        } catch (const std::exception &e) {
            spdlog::warn("session memory distillation failed: {}", e.what());
            result.status = std::string("Session distillation failed: ") + e.what();
        }

        return result;
    }

    [[nodiscard]]
    inline std::string render_prompt_memory_section(RuntimeMemory *memory, std::string_view user_input) {
        if (memory == nullptr) {
            return {};
        }

        const auto records = memory->recall_records(user_input, 8);
        if (records.empty()) {
            return {};
        }

        std::string memory_block = "\n\n<remembered-context>\n";
        memory_block.append("Use these remembered facts as quiet context. Do not recite them mechanically; let the current user request lead. "
                            "If a remembered fact may be stale or conflicts with the current request, ask or verify naturally.\n");

        std::size_t used = std::string{"<remembered-context>\n</remembered-context>"}.size();
        bool wrote_any = false;
        for (const auto &record : records) {
            std::string candidate;
            utils::format_to(candidate, "- [{}:{}] {}", magic_enum::enum_name(record.kind), record.key, record.content);
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
            return {};
        }
        memory_block.append("</remembered-context>");
        return memory_block;
    }

    [[nodiscard]]
    inline std::string build_system_prompt(std::string_view default_system_prompt, std::string_view skills_prompt, const ToolRegistry &tools, std::string_view memory_section) {
        std::vector<prompt::PromptSection> sections;
        sections.push_back(prompt::PromptSection{
            .id = "system.default",
            .kind = prompt::prompt_section_kind::static_section,
            .classification = prompt::prompt_section_classification::must_keep,
            .priority = 100,
            .content = std::string(default_system_prompt),
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
            deferred_section += "The following tools are already registered in the running process but their full schemas are deferred. "
                                "Use the `tool_search` tool to discover and enable them before use; do not use shell commands to search configuration files for them.\n";
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

        if (memory_section.empty()) {
            return prompt;
        }
        prompt.append(memory_section);
        return prompt;
    }

} // namespace orangutan::agent::detail
