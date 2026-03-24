#include "infra/config/secret-protection.hpp"
#include "infra/config/secret-fields.hpp"
#include "infra/files/file-io.hpp"

#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan {
namespace {

struct ParsedSecretAssignment {
    std::string prefix;
    std::string suffix;
    std::string literal_value;
    char quote = '"';
};

[[nodiscard]]
std::string trim_copy(std::string_view input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }

    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }

    return std::string(input.substr(begin, end - begin));
}

[[nodiscard]]
std::string strip_line_comment(std::string_view line) {
    const auto comment_pos = line.find('#');
    return comment_pos == std::string_view::npos ? trim_copy(line) : trim_copy(line.substr(0, comment_pos));
}

[[nodiscard]]
std::optional<ParsedSecretAssignment> parse_secret_assignment_line(const std::string &line, std::string_view key_name) {
    size_t pos = 0;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
        ++pos;
    }

    if (!line.substr(pos).starts_with(key_name)) {
        return std::nullopt;
    }
    pos += key_name.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '=') {
        return std::nullopt;
    }
    ++pos;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
        ++pos;
    }
    if (pos >= line.size() || (line[pos] != '"' && line[pos] != '\'')) {
        return std::nullopt;
    }

    const auto quote = line[pos];
    const auto literal_start = pos;
    ++pos;
    const auto value_start = pos;

    while (pos < line.size()) {
        if (quote == '"' && line[pos] == '\\') {
            pos += 2;
            continue;
        }
        if (line[pos] == quote) {
            break;
        }
        ++pos;
    }

    if (pos >= line.size()) {
        return std::nullopt;
    }

    return ParsedSecretAssignment{
        .prefix = line.substr(0, literal_start),
        .suffix = line.substr(pos + 1),
        .literal_value = line.substr(value_start, pos - value_start),
        .quote = quote,
    };
}

[[nodiscard]]
bool is_env_reference(std::string_view value) {
    return value.size() >= 4 && value.starts_with("${") && value.ends_with('}');
}

[[nodiscard]]
std::string read_config_file(const std::filesystem::path &path) {
    try {
        return fileio::read_file(path);
    } catch (const std::runtime_error &) {
        throw ConfigSecretProtectionError("Failed to read config file: " + path.string());
    }
}

void write_config_file(const std::filesystem::path &path, const std::string &content) {
    try {
        fileio::write_file_binary(path, content);
    } catch (const std::runtime_error &) {
        throw ConfigSecretProtectionError("Failed to write config file: " + path.string());
    }
}

[[nodiscard]]
std::filesystem::perms read_file_permissions(const std::filesystem::path &path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        throw ConfigSecretProtectionError("Failed to inspect config file permissions: " + ec.message());
    }
    return status.permissions();
}

[[nodiscard]]
std::string make_temp_filename(const std::filesystem::path &path) {
    thread_local std::mt19937_64 generator{std::random_device{}()};
    return std::format("{}.{:016x}.tmp", path.filename().string(), generator());
}

void prepare_parent_directory(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw ConfigSecretProtectionError("Failed to prepare config directory for rewrite: " + ec.message());
    }
}

void preserve_permissions(const std::filesystem::path &path, std::filesystem::perms permissions) {
    std::error_code ec;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, ec);
    if (ec) {
        std::filesystem::remove(path);
        throw ConfigSecretProtectionError("Failed to preserve config file permissions during rewrite: " + ec.message());
    }
}

} // namespace

ProtectConfigSecretsResult protect_config_file_secrets(const std::filesystem::path &path, std::string_view password) {
    if (password.empty()) {
        throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
    }

    const auto original = read_config_file(path);
    const auto original_permissions = read_file_permissions(path);
    const bool had_trailing_newline = !original.empty() && original.back() == '\n';

    std::istringstream input(original);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    if (original.empty()) {
        lines.emplace_back();
    }

    const ConfigSecretFieldSpec *active_field = nullptr;
    size_t protected_count = 0;

    for (auto &current_line : lines) {
        const auto cleaned = strip_line_comment(current_line);
        if (!cleaned.empty() && cleaned.front() == '[' && cleaned.back() == ']') {
            active_field = find_config_secret_field_for_section(cleaned);
            continue;
        }
        if (active_field == nullptr) {
            continue;
        }

        auto parsed = parse_secret_assignment_line(current_line, active_field->key_name);
        if (!parsed.has_value() || parsed->literal_value.empty() || is_env_reference(parsed->literal_value) || is_protected_config_secret(parsed->literal_value)) {
            continue;
        }

        const auto protected_value = protect_config_secret(parsed->literal_value, password, active_field->field_kind);
        current_line = parsed->prefix + parsed->quote + protected_value + parsed->quote + parsed->suffix;
        ++protected_count;
    }

    if (protected_count == 0) {
        return {};
    }

    std::string rebuilt;
    for (size_t index = 0; index < lines.size(); ++index) {
        rebuilt += lines[index];
        if (index + 1 < lines.size() || had_trailing_newline) {
            rebuilt.push_back('\n');
        }
    }

    auto backup_path = path;
    backup_path += ".bak";
    const auto temp_path = path.parent_path() / make_temp_filename(path);

    prepare_parent_directory(path.parent_path());

    std::error_code ec;
    std::filesystem::copy_file(path, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw ConfigSecretProtectionError("Failed to create config backup: " + ec.message());
    }

    try {
        write_config_file(temp_path, rebuilt);
        preserve_permissions(temp_path, original_permissions);
        std::filesystem::rename(temp_path, path, ec);
        if (ec) {
            std::filesystem::remove(temp_path);
            throw ConfigSecretProtectionError("Failed to replace config file atomically: " + ec.message());
        }
    } catch (...) {
        std::filesystem::remove(temp_path, ec);
        throw;
    }

    return {
        .modified = true,
        .protected_count = protected_count,
        .backup_path = backup_path,
    };
}

} // namespace orangutan
