#include <oran/config/config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <re2/re2.h>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::config {
namespace {

using json = ::nlohmann::ordered_json;
using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr auto kMaxInteger = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] Error config_error(std::string message, std::string path) {
  return Error::config(std::move(message)).with("path", std::move(path));
}

[[nodiscard]] std::string child_path(std::string_view base, std::string_view child) {
  if (base.empty() || base == "$") {
    return std::format("$.{}", child);
  }
  return std::format("{}.{}", base, child);
}

[[nodiscard]] std::string element_path(std::string_view base, std::size_t index) {
  return std::format("{}[{}]", base, index);
}

[[nodiscard]] bool valid_env_name(std::string_view name) {
  return !name.empty() &&
         std::ranges::all_of(name, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
}

[[nodiscard]] Result<std::string> expand_env_string(std::string_view input, std::string_view path) {
  auto output = std::string{};
  auto cursor = std::size_t{0};

  while (cursor < input.size()) {
    const auto start = input.find("${", cursor);
    if (start == std::string_view::npos) {
      output.append(input.substr(cursor));
      break;
    }

    output.append(input.substr(cursor, start - cursor));
    const auto end = input.find('}', start + 2);
    if (end == std::string_view::npos) {
      return std::unexpected(config_error("unterminated environment substitution", std::string{path}));
    }

    const auto expr = input.substr(start + 2, end - start - 2);
    const auto default_marker = expr.find(":-");
    const auto has_default = default_marker != std::string_view::npos;
    const auto name = has_default ? expr.substr(0, default_marker) : expr;
    const auto fallback = has_default ? expr.substr(default_marker + 2) : std::string_view{};
    if (!valid_env_name(name)) {
      return std::unexpected(
          config_error("invalid environment variable name", std::string{path}).with("name", std::string{name}));
    }

    const auto name_text = std::string{name};
    const auto* value = std::getenv(name_text.c_str());
    if (value != nullptr && (value[0] != '\0' || !has_default)) {
      output.append(value);
    } else if (has_default) {
      output.append(fallback);
    } else {
      return std::unexpected(config_error("missing environment variable", std::string{path}).with("name", name_text));
    }

    cursor = end + 1;
  }

  return output;
}

[[nodiscard]] Result<std::int64_t>
integer_value(const json& value, const std::string& path, std::int64_t min, std::int64_t max) {
  if (!value.is_number_integer()) {
    return std::unexpected(config_error("expected integer", path));
  }
  if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxInteger)) {
    return std::unexpected(config_error("integer is out of range", path));
  }
  const auto parsed = value.get<std::int64_t>();
  if (parsed < min) {
    return std::unexpected(
        config_error(min == 1 ? "expected positive integer" : "expected non-negative integer", path));
  }
  if (parsed > max) {
    return std::unexpected(config_error("integer is out of range", path).with("max", std::to_string(max)));
  }
  return parsed;
}

template <class T>
struct IntegerOf {
  using type = T;
};
template <class T>
struct IntegerOf<std::optional<T>> {
  using type = T;
};

struct ParseState {
  bool strict{false};
  std::vector<ConfigWarning> warnings{};
};

/// A view of one JSON object. Reads declare its recognized keys; the first
/// failure is kept and later reads are skipped. `finish` reports keys that no
/// read claimed, as warnings or, when strict, as the error.
class Fields {
public:
  Fields(const json& value, std::string path, ParseState& state)
      : value_{&value}, path_{std::move(path)}, state_{&state} {
    if (!value.is_object()) {
      fail(config_error("expected object", path_));
    }
  }

  [[nodiscard]] bool ok() const noexcept {
    return !error_.has_value();
  }
  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }
  [[nodiscard]] std::string path_of(std::string_view key) const {
    return child_path(path_, key);
  }
  [[nodiscard]] ParseState& state() const noexcept {
    return *state_;
  }
  [[nodiscard]] const json& value() const noexcept {
    return *value_;
  }

  void fail(Error error) {
    if (!error_) {
      error_ = std::move(error);
    }
  }

  /// Claim a key and return its value, or null when absent or after a failure.
  [[nodiscard]] const json* take(std::string_view key) {
    if (!ok()) {
      return nullptr;
    }
    known_.push_back(key);
    const auto it = value_->find(key);
    return it == value_->end() ? nullptr : &*it;
  }

  template <class Parse>
  void field(std::string_view key, Parse&& parse) {
    if (const auto* value = take(key)) {
      if (auto parsed = parse(*value, path_of(key)); !parsed) {
        fail(std::move(parsed).error());
      }
    }
  }

  void require(std::string_view key) {
    if (ok() && !value_->contains(key)) {
      fail(config_error("missing required field", path_of(key)));
    }
  }

  void boolean(std::string_view key, bool& out) {
    field(key, [&out](const json& value, const std::string& path) -> Result<void> {
      if (!value.is_boolean()) {
        return std::unexpected(config_error("expected boolean", path));
      }
      out = value.get<bool>();
      return {};
    });
  }

  template <class T>
  void integer(std::string_view key, T& out, std::int64_t min = 1, std::int64_t max = kMaxInteger) {
    field(key, [&](const json& value, const std::string& path) -> Result<void> {
      auto parsed = integer_value(value, path, min, max);
      if (!parsed) {
        return std::unexpected(std::move(parsed).error());
      }
      out = static_cast<typename IntegerOf<T>::type>(*parsed);
      return {};
    });
  }

  void price(std::string_view key, std::optional<double>& out) {
    field(key, [&out](const json& value, const std::string& path) -> Result<void> {
      if (!value.is_number()) {
        return std::unexpected(config_error("expected number", path));
      }
      const auto parsed = value.get<double>();
      if (!std::isfinite(parsed) || parsed < 0.0) {
        return std::unexpected(config_error("expected non-negative finite number", path));
      }
      out = parsed;
      return {};
    });
  }

  /// Strings expand `${NAME}` and `${NAME:-default}` environment references.
  template <class T>
  void string(std::string_view key, T& out, bool non_empty = false) {
    field(key, [&](const json& value, const std::string& path) -> Result<void> {
      auto parsed = string_value(value, path, non_empty);
      if (!parsed) {
        return std::unexpected(std::move(parsed).error());
      }
      out = std::move(*parsed);
      return {};
    });
  }

  void required_string(std::string_view key, std::string& out, bool non_empty = false) {
    require(key);
    string(key, out, non_empty);
  }

  void strings(std::string_view key, std::vector<std::string>& out, bool non_empty = false) {
    field(key, [&](const json& value, const std::string& path) -> Result<void> {
      auto parsed = string_array(value, path, non_empty);
      if (!parsed) {
        return std::unexpected(std::move(parsed).error());
      }
      out = std::move(*parsed);
      return {};
    });
  }

  /// Parse a nested object with `parse(Fields&, T&)` and finish it.
  template <class T, class Parse>
  void object(std::string_view key, T& out, Parse&& parse) {
    if (const auto* value = take(key)) {
      nested(*value, path_of(key), out, parse);
    }
  }

  template <class T, class Parse>
  void nested(const json& value, std::string path, T& out, Parse&& parse) {
    auto fields = Fields{value, std::move(path), *state_};
    if (fields.ok()) {
      parse(fields, out);
    }
    if (auto finished = fields.finish(); !finished) {
      fail(std::move(finished).error());
    }
  }

  /// Parse every member of a name-keyed object as `parse(name, Fields&, T&)`.
  template <class T, class Parse>
  void entries(std::string_view key, std::vector<T>& out, Parse&& parse) {
    const auto* map = take(key);
    if (map == nullptr) {
      return;
    }
    if (!map->is_object()) {
      fail(config_error("expected object", path_of(key)));
      return;
    }
    out.reserve(map->size());
    for (const auto& [name, value] : map->items()) {
      auto entry = T{.name = name};
      nested(value, child_path(path_of(key), name), entry, parse);
      if (!ok()) {
        return;
      }
      out.push_back(std::move(entry));
    }
  }

  [[nodiscard]] Result<void> finish() {
    if (!ok()) {
      return std::unexpected(std::move(*error_));
    }
    for (const auto& [key, _] : value_->items()) {
      if (std::ranges::contains(known_, std::string_view{key})) {
        continue;
      }
      auto path = path_of(key);
      if (state_->strict) {
        return std::unexpected(config_error("unknown config field", std::move(path)));
      }
      state_->warnings.push_back(ConfigWarning{.path = std::move(path), .message = "unknown config field"});
    }
    return {};
  }

private:
  [[nodiscard]] static Result<std::string> string_value(const json& value, const std::string& path, bool non_empty) {
    if (!value.is_string()) {
      return std::unexpected(config_error("expected string", path));
    }
    auto expanded = expand_env_string(value.get_ref<const std::string&>(), path);
    if (expanded && non_empty && expanded->empty()) {
      return std::unexpected(config_error("expected non-empty string", path));
    }
    return expanded;
  }

  [[nodiscard]] static Result<std::vector<std::string>>
  string_array(const json& value, const std::string& path, bool non_empty) {
    if (!value.is_array()) {
      return std::unexpected(config_error("expected array", path));
    }
    auto out = std::vector<std::string>{};
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
      auto item = string_value(value[i], element_path(path, i), non_empty);
      if (!item) {
        return std::unexpected(std::move(item).error());
      }
      out.push_back(std::move(*item));
    }
    return out;
  }

  const json* value_;
  std::string path_;
  ParseState* state_;
  std::vector<std::string_view> known_{};
  std::optional<Error> error_{};
};

void parse_tool_output(Fields& f, ToolOutputRuntimeConfig& out) {
  f.integer("max_text_bytes", out.max_text_bytes);
  f.integer("max_data_bytes", out.max_data_bytes);
}

void parse_tool_scheduler(Fields& f, ToolSchedulerRuntimeConfig& out) {
  f.integer("max_parallel_tools", out.max_parallel_tools);
  f.integer("per_call_timeout_ms", out.per_call_timeout_ms);
}

void parse_prompt(Fields& f, PromptRuntimeConfig& out) {
  f.field("active_tools", [&out](const json& value, const std::string& path) -> Result<void> {
    if (value.is_string()) {
      if (value.get_ref<const std::string&>() != "defaults") {
        return std::unexpected(
            config_error("expected \"defaults\" or array of tool names", path).with("value", value.get<std::string>()));
      }
      return {};
    }
    if (!value.is_array()) {
      return std::unexpected(config_error("expected \"defaults\" or array of tool names", path));
    }
    auto names = std::vector<std::string>{};
    for (std::size_t i = 0; i < value.size(); ++i) {
      const auto& item = value[i];
      if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
        return std::unexpected(config_error("expected non-empty string", element_path(path, i)));
      }
      names.push_back(item.get<std::string>());
    }
    out.active_tools = PromptActiveToolsConfig{.use_defaults = false, .tool_names = std::move(names)};
    return {};
  });
}

void parse_stream(Fields& f, StreamRuntimeConfig& out) {
  f.integer("max_bytes", out.max_bytes);
}

void parse_runtime(Fields& f, RuntimeConfig& out) {
  f.integer("workers", out.workers);
  f.integer("request_timeout_ms", out.request_timeout_ms);
  f.object("tool_output", out.tool_output, parse_tool_output);
  f.object("tool_scheduler", out.tool_scheduler, parse_tool_scheduler);
  f.object("prompt", out.prompt, parse_prompt);
  f.object("stream", out.stream, parse_stream);
}

void parse_trace(Fields& f, TraceConfig& out) {
  f.boolean("enabled", out.enabled);
}

void parse_hooks(Fields& f, HooksConfig& out) {
  f.integer("timeout_ms", out.timeout_ms);
}

void parse_recall(Fields& f, LongtermMemoryRecallConfig& out) {
  f.boolean("enabled", out.enabled);
  f.integer("limit", out.limit, 1, 20);
  f.strings("kinds", out.kinds, true);
  if (!f.ok() || !f.value().contains("kinds")) {
    return;
  }
  const auto path = f.path_of("kinds");
  if (out.kinds.empty()) {
    f.fail(config_error("long-term memory recall kinds must not be empty", path));
  }
  for (std::size_t i = 0; i < out.kinds.size(); ++i) {
    if (std::ranges::contains(std::span{out.kinds}.first(i), out.kinds[i])) {
      f.fail(config_error("long-term memory recall kind must be unique", element_path(path, i)));
    }
  }
}

void parse_memory(Fields& f, MemoryConfig& out) {
  f.object("longterm", out.longterm, [](Fields& longterm, LongtermMemoryConfig& config) {
    longterm.object("recall", config.recall, parse_recall);
  });
}

void parse_cache(Fields& f, std::optional<PromptCacheConfig>& out) {
  auto& cache = out.emplace();
  f.boolean("enabled", cache.enabled);
  f.integer("min_prefix_bytes", cache.min_prefix_bytes, 0);
}

void parse_pricing(Fields& f, ProviderPricingConfig& out) {
  f.price("input_per_million_usd", out.input_per_million_usd);
  f.price("output_per_million_usd", out.output_per_million_usd);
  f.price("cache_creation_per_million_usd", out.cache_creation_per_million_usd);
  f.price("cache_read_per_million_usd", out.cache_read_per_million_usd);
}

void parse_profile(Fields& f, ProfileConfig& out) {
  f.required_string("provider", out.provider);
  f.string("protocol", out.protocol, true);
  f.required_string("model", out.model);
  f.required_string("base_url", out.base_url);
  f.required_string("api_key_env", out.api_key_env);
  f.object("pricing", out.pricing, parse_pricing);
  f.integer("thinking_budget", out.thinking_budget, 1, std::numeric_limits<std::uint32_t>::max());
  f.object("cache", out.cache, parse_cache);
}

void parse_route(Fields& f, RouteConfig& out) {
  f.required_string("primary", out.primary_profile);
  f.strings("fallbacks", out.fallback_profiles);
}

void parse_rule(Fields& f, PermissionRuleConfig& out) {
  f.required_string("tool_pattern", out.tool_pattern, true);
  f.field("capability", [&out](const json& value, const std::string& path) -> Result<void> {
    if (!value.is_string()) {
      return std::unexpected(config_error("expected string", path));
    }
    const auto& text = value.get_ref<const std::string&>();
    auto parsed = core::parse_enum<core::Capability>(text);
    if (!parsed) {
      return std::unexpected(config_error("unknown capability spelling", path).with("capability", text));
    }
    out.capability = *parsed;
    return {};
  });
  f.string("input_pattern", out.input_pattern, true);
  if (f.ok() && out.input_pattern.has_value()) {
    // Keep configuration copyable by retaining only the validated source;
    // bootstrap compiles the owned runtime pattern during rule assembly.
    re2::RE2::Options options{re2::RE2::DefaultOptions};
    options.set_log_errors(false);
    const auto compiled = re2::RE2{*out.input_pattern, options};
    if (!compiled.ok()) {
      f.fail(config_error("invalid input_pattern regex", f.path_of("input_pattern"))
                 .with("regex_error", compiled.error()));
    }
  }
  f.integer("replay_max", out.replay_max, 0, std::numeric_limits<std::uint32_t>::max());
  f.integer("approval_ttl_seconds", out.approval_ttl_seconds, 0);
}

void parse_workspace(Fields& f, WorkspacePermissionsConfig& out) {
  f.strings("extra_read_roots", out.extra_read_roots);
  f.strings("extra_write_roots", out.extra_write_roots);
}

void parse_permissions(Fields& f, PermissionsConfig& out) {
  f.object("workspace", out.workspace, parse_workspace);
  // Rules keep their document order across verdict keys.
  for (const auto& [key, _] : f.value().items()) {
    const auto verdict = core::parse_enum<PermissionVerdict>(key);
    if (!verdict) {
      continue;
    }
    const auto* rules = f.take(key);
    if (rules == nullptr) {
      return;
    }
    const auto path = f.path_of(key);
    if (!rules->is_array()) {
      f.fail(config_error("expected array", path));
      return;
    }
    for (std::size_t i = 0; i < rules->size() && f.ok(); ++i) {
      auto rule = PermissionRuleConfig{.verdict = *verdict};
      f.nested((*rules)[i], element_path(path, i), rule, parse_rule);
      out.rules.push_back(std::move(rule));
    }
  }
}

}  // namespace

core::Result<Config> Config::parse(std::string_view contents, LoadOptions options) {
  try {
    const auto root = json::parse(contents.begin(), contents.end());
    auto state = ParseState{};
    auto config = Config{};
    auto fields = Fields{root, "$", state};
    fields.boolean("strict_config", config.strict_config_);
    state.strict = options.strict_unknown_fields || config.strict_config_;
    fields.object("runtime", config.runtime_, parse_runtime);
    fields.entries("profiles", config.profiles_, parse_profile);
    fields.entries("routes", config.routes_, parse_route);
    fields.object("trace", config.trace_, parse_trace);
    fields.object("hooks", config.hooks_, parse_hooks);
    fields.object("memory", config.memory_, parse_memory);
    fields.object("permissions", config.permissions_, parse_permissions);
    if (auto finished = fields.finish(); !finished) {
      return std::unexpected(std::move(finished).error());
    }
    config.warnings_ = std::move(state.warnings);
    return config;
  } catch (const json::parse_error& e) {
    return std::unexpected(Error::config("failed to parse config JSON").with("detail", e.what()));
  } catch (const json::exception& e) {
    return std::unexpected(Error::config("failed to read config JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(Error::internal("config parser failed").with("detail", e.what()));
  }
}

core::Result<Config> Config::load_file(std::string_view path, LoadOptions options) {
  if (path.empty()) {
    return std::unexpected(Error::invalid_argument("config path is empty"));
  }

  auto path_text = std::string{path};
  try {
    const auto fs_path = std::filesystem::path{path_text};
    auto ec = std::error_code{};
    if (!std::filesystem::exists(fs_path, ec)) {
      if (ec) {
        return std::unexpected(
            Error::io("failed to check config file").with("path", path_text).with("detail", ec.message()));
      }
      return std::unexpected(Error::not_found("config file not found").with("path", path_text));
    }

    // Cap the file size before reading to bound memory under a malformed or
    // hostile config file. 16 MiB is far above any plausible hand-authored
    // config; the loader is single-shot at startup so a tighter bound is
    // appropriate. See docs/SECURITY.md "Sandbox Posture".
    const auto file_size = std::filesystem::file_size(fs_path, ec);
    if (ec) {
      return std::unexpected(
          Error::io("failed to stat config file").with("path", path_text).with("detail", ec.message()));
    }
    if (file_size > options.max_bytes) {
      return std::unexpected(Error::invalid_argument("config file exceeds max_bytes")
                                 .with("path", path_text)
                                 .with("size", std::to_string(file_size))
                                 .with("max_bytes", std::to_string(options.max_bytes)));
    }

    auto input = std::ifstream{fs_path, std::ios::binary};
    if (!input) {
      return std::unexpected(Error::io("failed to open config file").with("path", path_text));
    }

    auto contents = std::string{};
    contents.reserve(static_cast<std::size_t>(file_size));
    contents.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) {
      return std::unexpected(Error::io("failed to read config file").with("path", path_text));
    }

    return parse(contents, options);
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(Error::io("failed to load config file").with("path", path_text).with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(Error::internal("config file load failed").with("path", path_text).with("detail", e.what()));
  }
}

}  // namespace orangutan::config
