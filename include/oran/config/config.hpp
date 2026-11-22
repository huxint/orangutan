#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>

namespace orangutan::config {

struct ConfigWarning {
  std::string path;
  std::string message;
};

struct ToolOutputRuntimeConfig {
  std::int64_t max_text_bytes{256 * 1024};
  std::int64_t max_data_bytes{1024 * 1024};

  friend bool operator==(const ToolOutputRuntimeConfig&, const ToolOutputRuntimeConfig&) = default;
};

/// Session tool bounds: four concurrent calls and a 60-second dispatch timeout.
struct ToolSchedulerRuntimeConfig {
  std::int64_t max_parallel_tools{4};
  std::int64_t per_call_timeout_ms{60000};

  friend bool operator==(const ToolSchedulerRuntimeConfig&, const ToolSchedulerRuntimeConfig&) = default;
};

struct PromptActiveToolsConfig {
  bool use_defaults{true};
  std::vector<std::string> tool_names{};

  friend bool operator==(const PromptActiveToolsConfig&, const PromptActiveToolsConfig&) = default;
};

struct PromptRuntimeConfig {
  PromptActiveToolsConfig active_tools{};

  friend bool operator==(const PromptRuntimeConfig&, const PromptRuntimeConfig&) = default;
};

/// Provider pricing in USD per one million tokens. Input and output prices are
/// separate because hosted models commonly price the two streams differently;
/// cache token prices are optional and default to the input token price when a
/// cost calculation consumes them.
struct ProviderPricingConfig {
  std::optional<double> input_per_million_usd{};
  std::optional<double> output_per_million_usd{};
  std::optional<double> cache_creation_per_million_usd{};
  std::optional<double> cache_read_per_million_usd{};

  [[nodiscard]] bool empty() const noexcept {
    return !input_per_million_usd.has_value() && !output_per_million_usd.has_value() &&
           !cache_creation_per_million_usd.has_value() && !cache_read_per_million_usd.has_value();
  }

  friend bool operator==(const ProviderPricingConfig&, const ProviderPricingConfig&) = default;
};

/// Byte bound for provider streaming responses. Mirrors the config loader's
/// own `max_bytes` cap on the stream consumption path: a provider stream may
/// hold at most this many wire bytes before the transfer is aborted with an
/// IO error, bounding memory regardless of wall-clock timeout.
struct StreamRuntimeConfig {
  std::int64_t max_bytes{16 * 1024 * 1024};

  friend bool operator==(const StreamRuntimeConfig&, const StreamRuntimeConfig&) = default;
};

struct RuntimeConfig {
  std::int64_t workers{4};
  std::int64_t request_timeout_ms{600000};
  ToolOutputRuntimeConfig tool_output{};
  ToolSchedulerRuntimeConfig tool_scheduler{};
  PromptRuntimeConfig prompt{};
  StreamRuntimeConfig stream{};
};

/// Optional per-profile prompt-cache policy, mapped by bootstrap into provider
/// values without importing the provider library into configuration parsing.
struct PromptCacheConfig {
  bool enabled{true};
  std::int64_t min_prefix_bytes{0};

  friend bool operator==(const PromptCacheConfig&, const PromptCacheConfig&) = default;
};

/// One provider profile from `profiles.<name>`. `provider` is the operator/vendor
/// label used for protocol aliases; optional `protocol` resolves as an exact
/// `provider::ProtocolKind` spelling so self-hosted gateways can select their
/// wire format without overloading the vendor label.
///
/// Bootstrap maps profile policy into owned provider values. Provider execution
/// selects thinking policy, and each protocol target applies its cache policy.
struct ProfileConfig {
  std::string name;
  std::string provider;
  std::optional<std::string> protocol;
  std::string model;
  std::string base_url;
  std::string api_key_env;
  ProviderPricingConfig pricing;
  std::optional<std::uint32_t> thinking_budget{};
  std::optional<PromptCacheConfig> cache{};
};

struct RouteConfig {
  std::string name;
  std::string primary_profile;
  std::vector<std::string> fallback_profiles{};
};

struct TraceConfig {
  bool enabled{true};

  friend bool operator==(const TraceConfig&, const TraceConfig&) = default;
};

struct HooksConfig {
  std::int64_t timeout_ms{2000};

  friend bool operator==(const HooksConfig&, const HooksConfig&) = default;
};

struct LongtermMemoryRecallConfig {
  bool enabled{true};
  std::int64_t limit{20};
  std::vector<std::string> kinds{};

  friend bool operator==(const LongtermMemoryRecallConfig&, const LongtermMemoryRecallConfig&) = default;
};

struct LongtermMemoryConfig {
  LongtermMemoryRecallConfig recall{};

  friend bool operator==(const LongtermMemoryConfig&, const LongtermMemoryConfig&) = default;
};

struct MemoryConfig {
  LongtermMemoryConfig longterm{};

  friend bool operator==(const MemoryConfig&, const MemoryConfig&) = default;
};

/// Verdict spelling that appears in `config.permissions.{allow,deny,ask}`.
/// Mirrors `permission::Verdict` but stays inside `oran-config` because the
/// dependency direction (config below permission) forbids importing the
/// permission header here. The configuration adapter in `oran-bootstrap`
/// maps `PermissionVerdict` to `permission::Verdict` one-to-one.
enum class PermissionVerdict : std::uint8_t {
  allow,
  deny,
  ask,
};

/// One config-side permission rule. `tool_pattern` is the `*`-glob the
/// `permission::RuleSet` matcher will consume. `capability`, when set, is
/// already resolved to a `core::Capability` value — unknown spellings fail
/// at load time so the materializer does not have to revalidate.
/// `input_pattern`, when set, is a re2 source pattern string. The loader
/// pre-validates the regex (compiles + discards) so syntactically invalid
/// patterns fail at config load with the offending JSON path, mirroring the
/// criterion 4 "invalid patterns at load time are reported" guarantee in
/// `docs/design-docs/permissions-and-hooks.md`. Bootstrap recompiles
/// the same pattern via `permission::InputPattern` when it assembles the
/// runtime `Rule`s. `replay_max` and `approval_ttl_seconds` carry the
/// per-rule approval-window policy (`docs/design-docs/permissions-and-hooks.md`
/// "Approval Signing": `replay_max=8`, `approval_ttl=3600s`). They are
/// optional at the config layer — when unset, the materializer keeps the
/// `permission::Rule` struct defaults, so an operator who omits the
/// fields gets the design-doc baseline.
struct PermissionRuleConfig {
  PermissionVerdict verdict{PermissionVerdict::deny};
  std::string tool_pattern;
  std::optional<core::Capability> capability{};
  std::optional<std::string> input_pattern{};
  std::optional<std::uint32_t> replay_max{};
  std::optional<std::int64_t> approval_ttl_seconds{};

  friend bool operator==(const PermissionRuleConfig&, const PermissionRuleConfig&) = default;
};

/// Workspace policy carried inside a permissions block. Extra read/write
/// roots widen the canonical workspace root after `tool::Workspace` builds
/// the resolver; the loader keeps the raw strings, leaving canonicalisation
/// and existence checks to `tool::Workspace::create` so config can be
/// parsed without touching the filesystem.
struct WorkspacePermissionsConfig {
  std::vector<std::string> extra_read_roots;
  std::vector<std::string> extra_write_roots;

  friend bool operator==(const WorkspacePermissionsConfig&, const WorkspacePermissionsConfig&) = default;
};

/// Rules collected from one permissions block (the global `permissions`
/// root). Rules appear in the JSON object's
/// iteration order so the operator's authoring intent survives the
/// materialize step (precedence is recovered by the runtime evaluator's
/// deny → allow → ask walk).
struct PermissionsConfig {
  std::vector<PermissionRuleConfig> rules;
  WorkspacePermissionsConfig workspace;

  friend bool operator==(const PermissionsConfig&, const PermissionsConfig&) = default;
};

struct LoadOptions {
  /// When enabled, unknown fields are rejected at the root plus typed nested
  /// config sections that already have a model (`profiles`, `pricing`,
  /// `routes`, `hooks`, `memory`, `permissions`, and `workspace`).
  /// Loose mode preserves them as `ConfigWarning` rows and otherwise ignores
  /// them.
  bool strict_unknown_fields{false};

  /// Hard cap on `load_file` size. The loader rejects files larger than this
  /// before any allocation, bounding memory cost under a malformed or hostile
  /// config. 16 MiB sits well above any plausible hand-authored configuration.
  std::uint64_t max_bytes{16ULL * 1024 * 1024};
};

class Config {
public:
  Config() = default;

  [[nodiscard]] static core::Result<Config> parse(std::string_view contents, LoadOptions options = {});
  [[nodiscard]] static core::Result<Config> load_file(std::string_view path, LoadOptions options = {});

  [[nodiscard]] bool strict_config() const noexcept {
    return strict_config_;
  }
  [[nodiscard]] const RuntimeConfig& runtime() const noexcept {
    return runtime_;
  }
  [[nodiscard]] std::span<const ProfileConfig> profiles() const noexcept {
    return std::span<const ProfileConfig>{profiles_};
  }
  [[nodiscard]] std::span<const RouteConfig> routes() const noexcept {
    return std::span<const RouteConfig>{routes_};
  }
  [[nodiscard]] const TraceConfig& trace() const noexcept {
    return trace_;
  }
  [[nodiscard]] const HooksConfig& hooks() const noexcept {
    return hooks_;
  }
  [[nodiscard]] const MemoryConfig& memory() const noexcept {
    return memory_;
  }
  [[nodiscard]] const PermissionsConfig& permissions() const noexcept {
    return permissions_;
  }
  [[nodiscard]] std::span<const ConfigWarning> warnings() const noexcept {
    return std::span<const ConfigWarning>{warnings_};
  }

private:
  bool strict_config_{false};
  RuntimeConfig runtime_{};
  std::vector<ProfileConfig> profiles_{};
  std::vector<RouteConfig> routes_{};
  TraceConfig trace_{};
  HooksConfig hooks_{};
  MemoryConfig memory_{};
  PermissionsConfig permissions_{};
  std::vector<ConfigWarning> warnings_{};
};

}  // namespace orangutan::config
