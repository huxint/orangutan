#include "permissions/approval-signature.hpp"

#include "types/content.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <vector>

namespace orangutan::permissions {

    namespace {

        std::string trim_copy(std::string_view value) {
            std::size_t start = 0;
            while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
                ++start;
            }

            std::size_t end = value.size();
            while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
                --end;
            }

            return std::string(value.substr(start, end - start));
        }

        std::string collapse_whitespace(std::string_view value) {
            std::string output;
            output.reserve(value.size());

            bool pending_space = false;
            for (const auto ch : value) {
                if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                    pending_space = !output.empty();
                    continue;
                }

                if (pending_space) {
                    output.push_back(' ');
                    pending_space = false;
                }
                output.push_back(ch);
            }

            return output;
        }

        std::optional<std::string> get_string_field(const nlohmann::json &input, std::string_view key) {
            const auto it = input.find(static_cast<std::string>(key));
            if (it == input.end() || !it->is_string()) {
                return std::nullopt;
            }

            auto value = trim_copy(it->get<std::string>());
            if (value.empty()) {
                return std::nullopt;
            }
            return value;
        }

        std::optional<std::string> first_string_field(const nlohmann::json &input, std::initializer_list<std::string_view> keys) {
            for (const auto key : keys) {
                if (const auto value = get_string_field(input, key); value.has_value()) {
                    return value;
                }
            }
            return std::nullopt;
        }

        std::string normalize_path(std::string_view raw) {
            if (raw.empty()) {
                return {};
            }

            try {
                const auto normalized = std::filesystem::path(std::string(raw)).lexically_normal().generic_string();
                return normalized.empty() ? std::string(raw) : normalized;
            } catch (...) {
                return std::string(raw);
            }
        }

        std::optional<std::string> first_path_field(const nlohmann::json &input, std::initializer_list<std::string_view> keys) {
            const auto value = first_string_field(input, keys);
            if (!value.has_value()) {
                return std::nullopt;
            }
            return normalize_path(*value);
        }

        std::vector<std::string> unique_sorted(std::vector<std::string> values) {
            values.erase(std::remove_if(values.begin(), values.end(), [](const std::string &value) {
                return value.empty();
            }),
                         values.end());
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
            return values;
        }

        std::string join_values(const std::vector<std::string> &values, std::string_view separator = "|") {
            std::string joined;
            for (const auto &value : values) {
                if (!joined.empty()) {
                    joined.append(separator);
                }
                joined.append(value);
            }
            return joined;
        }

        std::optional<RuleContent> exact_content(std::string value) {
            if (value.empty()) {
                return std::nullopt;
            }
            return RuleContent{
                .match_type = rule_match_type::exact,
                .pattern = std::move(value),
            };
        }

        ApprovalSignature build_signature(const ToolUse &call, std::optional<RuleContent> content, bool eligible = true, std::string downgrade_reason = {}) {
            return ApprovalSignature{
                .tool_name = call.name,
                .content = std::move(content),
                .always_allow_eligible = eligible && content.has_value(),
                .downgrade_reason = std::move(downgrade_reason),
            };
        }

        bool has_compound_shell_operator(std::string_view command) {
            bool in_single_quotes = false;
            bool in_double_quotes = false;
            bool escaped = false;

            for (std::size_t index = 0; index < command.size(); ++index) {
                const char ch = command[index];
                if (escaped) {
                    escaped = false;
                    continue;
                }

                if (ch == '\\') {
                    escaped = true;
                    continue;
                }

                if (ch == '\'' && !in_double_quotes) {
                    in_single_quotes = !in_single_quotes;
                    continue;
                }

                if (ch == '"' && !in_single_quotes) {
                    in_double_quotes = !in_double_quotes;
                    continue;
                }

                if (in_single_quotes || in_double_quotes) {
                    continue;
                }

                if (ch == ';' || ch == '<' || ch == '>') {
                    return true;
                }
                if ((ch == '&' || ch == '|') && index + 1 < command.size() && command[index + 1] == ch) {
                    return true;
                }
                if (ch == '|') {
                    return true;
                }
            }

            return false;
        }

        std::vector<std::string> extract_patch_paths(std::string_view patch) {
            std::vector<std::string> paths;
            std::istringstream stream{std::string(patch)};
            std::string line;

            constexpr std::string_view FILE_HEADER = "*** ";
            while (std::getline(stream, line)) {
                if (!line.starts_with(FILE_HEADER)) {
                    continue;
                }
                auto path = trim_copy(std::string_view(line).substr(FILE_HEADER.size()));
                if (!path.empty()) {
                    paths.push_back(normalize_path(path));
                }
            }

            return unique_sorted(std::move(paths));
        }

        std::vector<std::string> extract_read_paths(const nlohmann::json &input) {
            std::vector<std::string> paths;
            if (const auto path = first_path_field(input, {"path", "file_path"}); path.has_value()) {
                paths.push_back(*path);
            }

            const auto it = input.find("paths");
            if (it != input.end() && it->is_array()) {
                for (const auto &entry : *it) {
                    if (entry.is_string()) {
                        paths.push_back(normalize_path(entry.get<std::string>()));
                    }
                }
            }

            return unique_sorted(std::move(paths));
        }

        std::string op_identity_signature(const nlohmann::json &input, std::initializer_list<std::string_view> identity_keys) {
            std::vector<std::string> parts;
            if (const auto op = get_string_field(input, "op"); op.has_value()) {
                parts.push_back("op=" + *op);
            }

            for (const auto key : identity_keys) {
                if (const auto value = get_string_field(input, key); value.has_value()) {
                    parts.push_back(std::string(key) + "=" + *value);
                    break;
                }
            }

            return join_values(parts);
        }

        ApprovalSignature derive_shell_signature(const ToolUse &call) {
            const auto command = get_string_field(call.input, "command").value_or("");
            const auto normalized = collapse_whitespace(command);

            auto signature = build_signature(call, exact_content(normalized), !normalized.empty());
            if (normalized.empty()) {
                signature.downgrade_reason = "missing shell command";
                return signature;
            }

            if (has_compound_shell_operator(normalized)) {
                signature.always_allow_eligible = false;
                signature.downgrade_reason = "compound shell command";
            }
            return signature;
        }

        ApprovalSignature derive_file_signature(const ToolUse &call) {
            if (call.name == "read" || call.name.contains("file_read")) {
                return build_signature(call, exact_content(join_values(extract_read_paths(call.input))));
            }

            if (call.name == "write" || call.name.contains("file_write")) {
                return build_signature(call, exact_content(first_path_field(call.input, {"path", "file_path"}).value_or("")));
            }

            if (const auto path = first_path_field(call.input, {"path", "file_path"}); path.has_value()) {
                return build_signature(call, exact_content(*path));
            }

            if (const auto patch = get_string_field(call.input, "patch"); patch.has_value()) {
                return build_signature(call, exact_content(join_values(extract_patch_paths(*patch))));
            }

            return build_signature(call, std::nullopt, false, "no stable file target");
        }

        ApprovalSignature derive_memory_signature(const ToolUse &call) {
            if (call.name == "remember" || call.name == "memory_store" || call.name == "memory_update" || call.name == "forget" || call.name == "memory_forget") {
                return build_signature(call, exact_content("key=" + get_string_field(call.input, "key").value_or("")),
                                       get_string_field(call.input, "key").has_value());
            }

            if (call.name == "recall" || call.name == "memory_recall") {
                const auto mode = get_string_field(call.input, "mode").value_or("");
                if (mode == "category") {
                    return build_signature(call, exact_content("mode=category|value=" + get_string_field(call.input, "value").value_or("")));
                }
                return build_signature(call, exact_content("mode=" + mode), false, "free-text recall query");
            }

            if (call.name == "memory_list") {
                if (const auto category = get_string_field(call.input, "category"); category.has_value()) {
                    return build_signature(call, exact_content("category=" + *category));
                }
                return build_signature(call, exact_content("op=list"));
            }

            if (call.name == "memory_stats") {
                return build_signature(call, exact_content("op=stats"));
            }

            return build_signature(call, std::nullopt, false, "unrecognized memory tool");
        }

        ApprovalSignature derive_tool_specific_signature(const ToolUse &call) {
            if (call.name == "skill") {
                return build_signature(call, exact_content("name=" + get_string_field(call.input, "name").value_or("")),
                                       get_string_field(call.input, "name").has_value());
            }

            if (call.name == "tool_search") {
                const auto query = get_string_field(call.input, "query").value_or("");
                if (!query.starts_with("select:")) {
                    return build_signature(call, exact_content("mode=search"), false, "free-form tool search query");
                }

                std::vector<std::string> names;
                std::istringstream stream(query.substr(std::string("select:").size()));
                for (std::string token; std::getline(stream, token, ',');) {
                    auto trimmed = trim_copy(token);
                    if (!trimmed.empty()) {
                        names.push_back(trimmed);
                    }
                }
                names = unique_sorted(std::move(names));
                return build_signature(call, exact_content("select=" + join_values(names, ",")), !names.empty());
            }

            if (call.name == "task") {
                return build_signature(call, exact_content(op_identity_signature(call.input, {"id", "name"})));
            }

            if (call.name == "heartbeat") {
                return build_signature(call, exact_content(op_identity_signature(call.input, {"id", "name"})));
            }

            if (call.name == "inbox") {
                return build_signature(call, exact_content(op_identity_signature(call.input, {"id"})));
            }

            if (call.name == "message_attachments") {
                const auto op = get_string_field(call.input, "op").value_or("list");
                if (op != "download") {
                    return build_signature(call, exact_content("op=" + op));
                }
                const auto target_path = first_path_field(call.input, {"target_path"});
                return build_signature(call,
                                       exact_content(target_path.has_value() ? "op=download|target_path=" + *target_path : "op=download"),
                                       false,
                                       "attachment download is message-specific");
            }

            if (call.name == "process_list") {
                return build_signature(call, exact_content("op=list"));
            }

            if (call.name == "process_poll" || call.name == "process_kill") {
                return build_signature(call, exact_content("process_id=" + get_string_field(call.input, "process_id").value_or("")),
                                       get_string_field(call.input, "process_id").has_value());
            }

            if (call.name == "agent_spawn") {
                std::vector<std::string> parts;
                if (const auto agent_key = get_string_field(call.input, "agent_key"); agent_key.has_value()) {
                    parts.push_back("agent_key=" + *agent_key);
                }
                if (const auto team = get_string_field(call.input, "team"); team.has_value()) {
                    parts.push_back("team=" + *team);
                }
                return build_signature(call, exact_content(join_values(parts)), !parts.empty());
            }

            if (call.name == "agent_send_message") {
                if (const auto run_id = get_string_field(call.input, "run_id"); run_id.has_value()) {
                    return build_signature(call, exact_content("run_id=" + *run_id));
                }
                if (const auto to = get_string_field(call.input, "to"); to.has_value()) {
                    return build_signature(call, exact_content("to=" + *to));
                }
                return build_signature(call, std::nullopt, false, "no stable message recipient");
            }

            if (call.name == "agent_stop") {
                return build_signature(call, exact_content("run_id=" + get_string_field(call.input, "run_id").value_or("")),
                                       get_string_field(call.input, "run_id").has_value());
            }

            if (call.name == "team_create") {
                return build_signature(call, exact_content("name=" + get_string_field(call.input, "name").value_or("")),
                                       get_string_field(call.input, "name").has_value());
            }

            if (call.name == "team_delete") {
                return build_signature(call, exact_content("team_id=" + get_string_field(call.input, "team_id").value_or("")),
                                       get_string_field(call.input, "team_id").has_value());
            }

            return build_signature(call, std::nullopt, false, "no explicit canonical signature");
        }

    } // namespace

    ApprovalSignature derive_approval_signature(const ToolUse &call) {
        if (call.name == "shell") {
            return derive_shell_signature(call);
        }

        if (call.name == "read" || call.name == "write" || call.name == "edit" || call.name.contains("file_read") || call.name.contains("file_write")
            || call.name.contains("file_edit")) {
            return derive_file_signature(call);
        }

        if (call.name == "remember" || call.name == "memory_store" || call.name == "memory_update" || call.name == "forget" || call.name == "memory_forget"
            || call.name == "recall" || call.name == "memory_recall" || call.name == "memory_list" || call.name == "memory_stats") {
            return derive_memory_signature(call);
        }

        return derive_tool_specific_signature(call);
    }

    std::string approval_match_content(const ToolUse &call) {
        const auto signature = derive_approval_signature(call);
        return signature.content.has_value() ? signature.content->pattern : std::string{};
    }

    std::optional<PermissionRule> make_session_allow_rule(const ToolUse &call) {
        const auto signature = derive_approval_signature(call);
        if (!signature.always_allow_eligible) {
            return std::nullopt;
        }

        return PermissionRule{
            .source = permission_rule_source::session,
            .behavior = permission_behavior::allow,
            .tool_name = signature.tool_name,
            .content = signature.content,
        };
    }

} // namespace orangutan::permissions
