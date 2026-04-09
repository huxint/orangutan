#include "permissions/approval-signature.hpp"

#include "types/content.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::permissions {

    namespace {
        constexpr std::string_view FILE_HEADER = "*** ";

        std::string collapse_whitespace(std::string_view value) {
            std::string output;
            output.reserve(value.size());

            bool spaced = false;
            for (char ch : value) {
                if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                    spaced = !output.empty();
                } else {
                    if (std::exchange(spaced, false)) {
                        output.push_back(' ');
                    }
                    output.push_back(ch);
                }
            }

            return output;
        }

        std::optional<std::string> get_string_field(const nlohmann::json &input, std::string_view key) {
            const auto it = input.find(static_cast<std::string>(key));
            if (it == input.end() || !it->is_string()) {
                return std::nullopt;
            }

            auto value = utils::trim_copy(it->get<std::string>());
            if (value.empty()) {
                return std::nullopt;
            }
            return static_cast<std::string>(value);
        }

        std::string normalize_path(std::string_view raw) {
            try {
                return std::filesystem::path(raw).lexically_normal().generic_string();
            } catch (...) {
                return std::string{raw};
            }
        }

        std::optional<std::string> first_path_field(const nlohmann::json &input, std::initializer_list<std::string_view> keys) {
            for (auto key : keys) {
                if (auto value = get_string_field(input, key)) {
                    return normalize_path(*value);
                }
            }
            return std::nullopt;
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
            enum class quote : char {
                none = 0,
                single = '\'',
                dbl = '"',
            };

            constexpr std::string_view METAS = ";<>|";

            auto state = quote::none;

            for (std::size_t i = 0; i < command.size(); ++i) {
                const char ch = command[i];

                if (ch == '\\') {
                    ++i;
                    continue;
                }

                if (state != quote::none) {
                    if (ch == std::to_underlying(state)) {
                        state = quote::none;
                    }
                    continue;
                }

                if (ch == '\'' || ch == '"') {
                    state = static_cast<quote>(ch);
                    continue;
                }

                if (METAS.contains(ch) || (ch == '&' && i + 1 < command.size() && command[i + 1] == '&')) {
                    return true;
                }
            }

            return false;
        }

        std::string extract_patch_paths_signature(std::string_view patch) {
            auto to_sv = [](auto &&r) -> std::string_view {
                return {std::ranges::data(r), std::ranges::size(r)};
            };

            auto paths = patch | std::views::split('\n') | std::views::transform(to_sv) | std::views::filter([](std::string_view line) {
                             return line.starts_with(FILE_HEADER);
                         }) |
                         std::views::transform([](std::string_view line) {
                             return normalize_path(utils::trim_copy(line.substr(FILE_HEADER.size())));
                         }) |
                         std::views::filter([](const std::string &path) {
                             return !path.empty();
                         }) |
                         std::ranges::to<std::vector<std::string_view>>();

            std::ranges::sort(paths);
            auto [new_end, old_end] = std::ranges::unique(paths);
            paths.erase(new_end, old_end);

            return paths | std::views::join_with('|') | std::ranges::to<std::string>();
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
