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

        std::optional<RuleContent> exact_content(std::string value) {
            if (value.empty()) {
                return std::nullopt;
            }
            return RuleContent{
                .match_type = rule_match_type::exact,
                .pattern = std::move(value),
            };
        }

        ApprovalSignature build_signature(std::optional<RuleContent> content, bool eligible = true, std::string downgrade_reason = {}) {
            return ApprovalSignature{
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

        std::string extract_patch_paths_signature(std::string_view patch) {
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

            paths.erase(std::remove_if(paths.begin(), paths.end(), [](const std::string &value) {
                return value.empty();
            }),
                        paths.end());
            std::sort(paths.begin(), paths.end());
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

            std::string joined;
            for (const auto &path : paths) {
                if (!joined.empty()) {
                    joined.push_back('|');
                }
                joined.append(path);
            }
            return joined;
        }

        ApprovalSignature derive_shell_signature(const ToolUse &call) {
            const auto command = get_string_field(call.input, "command").value_or("");
            const auto normalized = collapse_whitespace(command);

            auto signature = build_signature(exact_content(normalized), !normalized.empty());
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

        ApprovalSignature derive_write_signature(const ToolUse &call) {
            return build_signature(exact_content(first_path_field(call.input, {"path", "file_path"}).value_or("")));
        }

        ApprovalSignature derive_edit_signature(const ToolUse &call) {
            if (const auto path = first_path_field(call.input, {"path", "file_path"}); path.has_value()) {
                return build_signature(exact_content(*path));
            }

            if (const auto patch = get_string_field(call.input, "patch"); patch.has_value()) {
                return build_signature(exact_content(extract_patch_paths_signature(*patch)));
            }

            return build_signature(std::nullopt, false, "no stable edit target");
        }

    } // namespace

    ApprovalSignature derive_approval_signature(const ToolUse &call) {
        if (call.name == "shell") {
            return derive_shell_signature(call);
        }
        if (call.name == "write" || call.name.contains("file_write")) {
            return derive_write_signature(call);
        }
        if (call.name == "edit" || call.name.contains("file_edit")) {
            return derive_edit_signature(call);
        }

        return build_signature(std::nullopt, false, "tool does not use canonical approval signatures");
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
            .tool_name = call.name,
            .content = signature.content,
        };
    }

} // namespace orangutan::permissions
