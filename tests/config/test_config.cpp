#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace config = orangutan::config;
namespace core = orangutan::core;

namespace {

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(std::string name) : name_(std::move(name)) {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
      unsetenv(name_.c_str());
    }
  }

  ~ScopedUnsetEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

constexpr auto kMinimalConfig = R"json({
  "runtime": {
    "workers": 2,
    "request_timeout_ms": 1500,
    "tool_output": {
      "max_text_bytes": 4096,
      "max_data_bytes": 8192
    },
    "prompt": {
      "active_tools": [
        "FileRead",
        "MemoryRemember"
      ]
    }
  },
  "trace": {
    "enabled": false
  },
  "hooks": {
    "timeout_ms": 1234
  },
  "memory": {
    "longterm": {
      "recall": {
        "enabled": true,
        "limit": 3
      }
    }
  },
  "profiles": {
    "default": {
      "provider": "openai",
      "protocol": "openai_responses",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "pricing": {
        "input_per_million_usd": 1.25,
        "output_per_million_usd": 10.0,
        "cache_creation_per_million_usd": 1.5,
        "cache_read_per_million_usd": 0.125
      }
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": [
        "local"
      ]
    }
  }
})json";

std::string example_config_path() {
  constexpr auto candidates = std::array<std::string_view, 5>{
      "config.example.json",
      "../config.example.json",
      "../../config.example.json",
      "../../../config.example.json",
      "../../../../config.example.json",
  };

  for (const auto candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      return std::string{candidate};
    }
  }
  return {};
}

}  // namespace

TEST_CASE("Config::parse returns typed config values", "[unit][config]") {
  auto result = config::Config::parse(kMinimalConfig);

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->strict_config());
  REQUIRE(result->runtime().workers == 2);
  REQUIRE(result->runtime().request_timeout_ms == 1500);
  REQUIRE(result->runtime().tool_output.max_text_bytes == 4096);
  REQUIRE(result->runtime().tool_output.max_data_bytes == 8192);
  REQUIRE_FALSE(result->runtime().prompt.active_tools.use_defaults);
  REQUIRE(result->runtime().prompt.active_tools.tool_names == std::vector<std::string>{"FileRead", "MemoryRemember"});

  REQUIRE_FALSE(result->trace().enabled);

  REQUIRE(result->hooks().timeout_ms == 1234);
  REQUIRE(result->memory().longterm.recall.enabled);
  REQUIRE(result->memory().longterm.recall.limit == 3);

  REQUIRE(result->profiles().size() == 1);
  REQUIRE(result->profiles()[0].name == "default");
  REQUIRE(result->profiles()[0].provider == "openai");
  REQUIRE(result->profiles()[0].protocol == std::optional<std::string>{"openai_responses"});
  REQUIRE(result->profiles()[0].model == "gpt-5.5");
  REQUIRE(result->profiles()[0].base_url == "https://api.openai.com/v1");
  REQUIRE(result->profiles()[0].api_key_env == "OPENAI_API_KEY");
  REQUIRE(result->profiles()[0].pricing.input_per_million_usd == std::optional<double>{1.25});
  REQUIRE(result->profiles()[0].pricing.output_per_million_usd == std::optional<double>{10.0});
  REQUIRE(result->profiles()[0].pricing.cache_creation_per_million_usd == std::optional<double>{1.5});
  REQUIRE(result->profiles()[0].pricing.cache_read_per_million_usd == std::optional<double>{0.125});

  REQUIRE(result->routes().size() == 1);
  REQUIRE(result->routes()[0].name == "default");
  REQUIRE(result->routes()[0].primary_profile == "default");
  REQUIRE(result->routes()[0].fallback_profiles == std::vector<std::string>{"local"});
}

TEST_CASE("Config::parse reads per-profile thinking_budget and cache policy", "[unit][config]") {
  SECTION("absent fields stay unset") {
    auto result = config::Config::parse(
        R"json({"profiles": {"default": {"provider": "anthropic", "model": "m", "base_url": "u", "api_key_env": "K"}}})json");
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->profiles()[0].thinking_budget.has_value());
    REQUIRE_FALSE(result->profiles()[0].cache.has_value());
  }
  SECTION("present fields parse") {
    auto result = config::Config::parse(R"json({
      "profiles": {
        "default": {
          "provider": "anthropic",
          "model": "m",
          "base_url": "u",
          "api_key_env": "K",
          "thinking_budget": 4096,
          "cache": {"enabled": false, "min_prefix_bytes": 1024}
        }
      }
    })json");
    REQUIRE(result.has_value());
    REQUIRE(result->profiles()[0].thinking_budget == std::optional<std::uint32_t>{4096});
    REQUIRE(result->profiles()[0].cache.has_value());
    REQUIRE_FALSE(result->profiles()[0].cache->enabled);
    REQUIRE(result->profiles()[0].cache->min_prefix_bytes == 1024);
  }
  SECTION("partial cache object uses defaults for absent keys") {
    auto result = config::Config::parse(R"json({
      "profiles": {"default": {"provider": "anthropic", "model": "m", "base_url": "u", "api_key_env": "K",
                               "cache": {"min_prefix_bytes": 64}}}
    })json");
    REQUIRE(result.has_value());
    REQUIRE(result->profiles()[0].cache->enabled);
    REQUIRE(result->profiles()[0].cache->min_prefix_bytes == 64);
  }
  SECTION("zero min_prefix_bytes is accepted") {
    auto result = config::Config::parse(R"json({
      "profiles": {"default": {"provider": "anthropic", "model": "m", "base_url": "u", "api_key_env": "K",
                               "cache": {"min_prefix_bytes": 0}}}
    })json");
    REQUIRE(result.has_value());
    REQUIRE(result->profiles()[0].cache->min_prefix_bytes == 0);
  }
  SECTION("non-positive thinking_budget and negative floor are rejected") {
    auto budget = config::Config::parse(R"json({
      "profiles": {"default": {"provider": "anthropic", "model": "m", "base_url": "u", "api_key_env": "K",
                               "thinking_budget": 0}}
    })json");
    REQUIRE_FALSE(budget.has_value());
    auto floor = config::Config::parse(R"json({
      "profiles": {"default": {"provider": "anthropic", "model": "m", "base_url": "u", "api_key_env": "K",
                               "cache": {"min_prefix_bytes": -1}}}
    })json");
    REQUIRE_FALSE(floor.has_value());
  }
}

TEST_CASE("Config::parse reads runtime.stream.max_bytes", "[unit][config]") {
  SECTION("absent field keeps the 16 MiB default") {
    auto result = config::Config::parse(R"json({})json");
    REQUIRE(result.has_value());
    REQUIRE(result->runtime().stream.max_bytes == 16 * 1024 * 1024);
  }
  SECTION("override parses") {
    auto result = config::Config::parse(R"json({"runtime": {"stream": {"max_bytes": 4096}}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->runtime().stream.max_bytes == 4096);
  }
  SECTION("non-positive values are rejected") {
    auto result = config::Config::parse(R"json({"runtime": {"stream": {"max_bytes": 0}}})json");
    REQUIRE_FALSE(result.has_value());
  }
}

TEST_CASE("Config::parse recursively substitutes environment variables", "[unit][config]") {
  ScopedEnv model{"ORAN_CONFIG_TEST_MODEL", "model-from-env"};
  ScopedUnsetEnv base{"ORAN_CONFIG_TEST_BASE"};

  auto result = config::Config::parse(R"json({
  "runtime": {},
  "profiles": {
    "default": {
      "provider": "local",
      "model": "${ORAN_CONFIG_TEST_MODEL}",
      "base_url": "${ORAN_CONFIG_TEST_BASE:-http://127.0.0.1:8080}",
      "api_key_env": "LOCAL_API_KEY"
    }
  }
})json");

  REQUIRE(result.has_value());

  REQUIRE(result->profiles()[0].model == "model-from-env");
  REQUIRE(result->profiles()[0].base_url == "http://127.0.0.1:8080");
}

TEST_CASE("Config::parse reports config errors without throwing", "[unit][config]") {
  SECTION("invalid JSON") {
    auto result = config::Config::parse("{");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("missing environment variable") {
    ScopedUnsetEnv missing{"ORAN_CONFIG_TEST_MISSING"};
    auto result = config::Config::parse(R"json(
{
  "profiles": {
    "default": {
      "provider": "local",
      "model": "${ORAN_CONFIG_TEST_MISSING}",
      "base_url": "http://127.0.0.1:8080",
      "api_key_env": "LOCAL_API_KEY"
    }
  }
}
)json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("typed value mismatch") {
    auto result = config::Config::parse(R"json({"trace": {"enabled": "yes"}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("integer out of range") {
    auto result = config::Config::parse(R"json({"runtime": {"workers": 18446744073709551615}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse warns or fails on unknown root fields", "[unit][config]") {
  ScopedUnsetEnv missing{"ORAN_CONFIG_TEST_MISSING"};

  auto loose = config::Config::parse(R"json({"future": true})json");
  REQUIRE(loose.has_value());
  REQUIRE(loose->warnings().size() == 1);
  REQUIRE(loose->warnings()[0].path == "$.future");

  auto loose_with_env = config::Config::parse(R"json({"future": "${ORAN_CONFIG_TEST_MISSING}"})json");
  REQUIRE(loose_with_env.has_value());
  REQUIRE(loose_with_env->warnings().size() == 1);
  REQUIRE(loose_with_env->warnings()[0].path == "$.future");

  auto strict_by_option =
      config::Config::parse(R"json({"future": true})json", config::LoadOptions{.strict_unknown_fields = true});
  REQUIRE_FALSE(strict_by_option.has_value());
  REQUIRE(strict_by_option.error().kind() == core::ErrorKind::config);

  auto strict_by_config = config::Config::parse(R"json({"strict_config": true, "future": true})json");
  REQUIRE_FALSE(strict_by_config.has_value());
  REQUIRE(strict_by_config.error().kind() == core::ErrorKind::config);
}

TEST_CASE("Config::parse warns or fails on unknown nested provider and hook fields", "[unit][config]") {
  SECTION("unknown profile field warns in loose mode") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "notes": "operator-only"
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.profiles.default.notes");
    REQUIRE(result->warnings()[0].message == "unknown config field");
  }

  SECTION("unknown pricing field warns in loose mode") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "pricing": {
        "input_per_million_usd": 1.25,
        "discount_code": "future"
      }
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.profiles.default.pricing.discount_code");
    REQUIRE(result->warnings()[0].message == "unknown config field");
  }

  SECTION("unknown route field warns in loose mode") {
    auto result = config::Config::parse(R"json({
  "routes": {
    "default": {
      "primary": "main",
      "fallbacks": [],
      "sticky": true
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.routes.default.sticky");
    REQUIRE(result->warnings()[0].message == "unknown config field");
  }

  SECTION("unknown hook field warns in loose mode") {
    auto result = config::Config::parse(R"json({
  "hooks": {
    "timeout_ms": 75,
    "sink_scripts": []
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.hooks.sink_scripts");
    REQUIRE(result->warnings()[0].message == "unknown config field");
  }

  SECTION("unknown memory recall field warns in loose mode") {
    auto result = config::Config::parse(R"json({
  "memory": {
    "longterm": {
      "recall": {
        "enabled": true,
        "ranking_strategy": "lexical"
      }
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.memory.longterm.recall.ranking_strategy");
    REQUIRE(result->warnings()[0].message == "unknown config field");
  }

  SECTION("unknown nested field fails under strict_config") {
    auto result = config::Config::parse(R"json({
  "strict_config": true,
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "notes": "operator-only"
    }
  }
})json");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown nested field fails under strict load option") {
    auto result = config::Config::parse(R"json({
  "routes": {
    "default": {
      "primary": "main",
      "fallbacks": [],
      "sticky": true
    }
  }
})json",
                                        config::LoadOptions{.strict_unknown_fields = true});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown memory field fails under strict load option") {
    auto result = config::Config::parse(R"json({
  "memory": {
    "longterm": {
      "recall": {
        "enabled": true,
        "ranking_strategy": "lexical"
      }
    }
  }
})json",
                                        config::LoadOptions{.strict_unknown_fields = true});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown memory retention field fails under strict load option") {
    auto result = config::Config::parse(R"json({
  "memory": {
    "longterm": {
      "retention": {
        "delete_after_unused_days": 365
      }
    }
  }
})json",
                                        config::LoadOptions{.strict_unknown_fields = true});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::load_file accepts the checked-in example config", "[unit][config]") {
  ScopedUnsetEnv default_model{"ORAN_DEFAULT_MODEL"};
  const auto path = example_config_path();
  REQUIRE_FALSE(path.empty());
  auto result = config::Config::load_file(path);

  REQUIRE(result.has_value());
  REQUIRE(result->profiles().size() == 1);
  REQUIRE(result->profiles()[0].name == "default");
  REQUIRE(result->profiles()[0].model == "claude-3-5-sonnet-latest");
  REQUIRE(result->profiles()[0].protocol == std::optional<std::string>{"anthropic_messages"});
  REQUIRE(result->profiles()[0].pricing.input_per_million_usd == std::optional<double>{3.0});
  REQUIRE(result->profiles()[0].pricing.output_per_million_usd == std::optional<double>{15.0});
  REQUIRE(result->profiles()[0].pricing.cache_creation_per_million_usd == std::optional<double>{3.75});
  REQUIRE(result->profiles()[0].pricing.cache_read_per_million_usd == std::optional<double>{0.3});
  REQUIRE(result->routes().size() == 1);
  REQUIRE(result->runtime().tool_output.max_text_bytes == 262144);
  REQUIRE(result->runtime().tool_output.max_data_bytes == 1048576);
  REQUIRE(result->runtime().tool_scheduler.max_parallel_tools == 4);
  REQUIRE(result->runtime().tool_scheduler.per_call_timeout_ms == 60000);
  REQUIRE(result->runtime().prompt.active_tools.use_defaults);
  REQUIRE(result->runtime().prompt.active_tools.tool_names.empty());
  REQUIRE(result->trace().enabled);

  REQUIRE(result->hooks().timeout_ms == 2000);
  REQUIRE(result->memory().longterm.recall.enabled);
  REQUIRE(result->memory().longterm.recall.limit == 20);

  REQUIRE(result->permissions().rules.size() == 6);
}

TEST_CASE("Config::parse validates optional provider profile pricing", "[unit][config][profiles]") {
  SECTION("non-object pricing") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "pricing": true
    }
  }
})json");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("negative price") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY",
      "pricing": {
        "input_per_million_usd": -1.0
      }
    }
  }
})json");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse validates optional provider profile protocol field", "[unit][config][profiles]") {
  SECTION("non-string protocol") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "protocol": false,
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    }
  }
})json");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty protocol") {
    auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "openai",
      "protocol": "",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    }
  }
})json");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts runtime.tool_output byte caps", "[unit][config][runtime]") {
  auto result = config::Config::parse(R"json({
  "runtime": {
    "tool_output": {
      "max_text_bytes": 1234,
      "max_data_bytes": 5678
    }
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE(result->runtime().tool_output.max_text_bytes == 1234);
  REQUIRE(result->runtime().tool_output.max_data_bytes == 5678);
}

TEST_CASE("Config::parse rejects malformed runtime.tool_output caps", "[unit][config][runtime]") {
  SECTION("non-object block") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_output": []}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-integer text cap") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_output": {"max_text_bytes": "large"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("zero data cap") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_output": {"max_data_bytes": 0}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts runtime.tool_scheduler knobs", "[unit][config][runtime]") {
  auto result = config::Config::parse(R"json({
  "runtime": {
    "tool_scheduler": {
      "max_parallel_tools": 8,
      "per_call_timeout_ms": 1500
    }
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE(result->runtime().tool_scheduler.max_parallel_tools == 8);
  REQUIRE(result->runtime().tool_scheduler.per_call_timeout_ms == 1500);
}

TEST_CASE("Config::parse defaults runtime.tool_scheduler when the block is absent", "[unit][config][runtime]") {
  auto result = config::Config::parse(R"json({"runtime": {}})json");
  REQUIRE(result.has_value());
  REQUIRE(result->runtime().tool_scheduler.max_parallel_tools == 4);
  REQUIRE(result->runtime().tool_scheduler.per_call_timeout_ms == 60000);
}

TEST_CASE("Config::parse rejects malformed runtime.tool_scheduler knobs", "[unit][config][runtime]") {
  SECTION("non-object block") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_scheduler": []}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("zero parallelism") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_scheduler": {"max_parallel_tools": 0}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-integer timeout") {
    auto result = config::Config::parse(R"json({"runtime": {"tool_scheduler": {"per_call_timeout_ms": "soon"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts runtime.prompt active tools", "[unit][config][runtime][prompt]") {
  SECTION("defaults sentinel") {
    auto result = config::Config::parse(R"json({
  "runtime": {
    "prompt": {
      "active_tools": "defaults"
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE(result->runtime().prompt.active_tools.use_defaults);
    REQUIRE(result->runtime().prompt.active_tools.tool_names.empty());
  }

  SECTION("explicit allowlist") {
    auto result = config::Config::parse(R"json({
  "runtime": {
    "prompt": {
      "active_tools": ["FileRead", "FileEdit", "MemoryRemember"]
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->runtime().prompt.active_tools.use_defaults);
    REQUIRE(result->runtime().prompt.active_tools.tool_names ==
            std::vector<std::string>{"FileRead", "FileEdit", "MemoryRemember"});
  }

  SECTION("empty explicit allowlist") {
    auto result = config::Config::parse(R"json({
  "runtime": {
    "prompt": {
      "active_tools": []
    }
  }
})json");

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->runtime().prompt.active_tools.use_defaults);
    REQUIRE(result->runtime().prompt.active_tools.tool_names.empty());
  }
}

TEST_CASE("Config::parse rejects malformed runtime.prompt active tools", "[unit][config][runtime][prompt]") {
  SECTION("non-object prompt block") {
    auto result = config::Config::parse(R"json({"runtime": {"prompt": []}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown sentinel") {
    auto result = config::Config::parse(R"json({"runtime": {"prompt": {"active_tools": "all"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-array active_tools") {
    auto result = config::Config::parse(R"json({"runtime": {"prompt": {"active_tools": {"name": "FileRead"}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-string tool name") {
    auto result = config::Config::parse(R"json({"runtime": {"prompt": {"active_tools": ["FileRead", 42]}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty tool name") {
    auto result = config::Config::parse(R"json({"runtime": {"prompt": {"active_tools": ["FileRead", ""]}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts trace policy", "[unit][config][trace]") {
  auto result = config::Config::parse(R"json({
  "trace": {
    "enabled": false
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->trace().enabled);
}

TEST_CASE("Config::parse rejects malformed trace policy", "[unit][config][trace]") {
  SECTION("non-object trace block") {
    auto result = config::Config::parse(R"json({"trace": []})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-boolean enabled") {
    auto result = config::Config::parse(R"json({"trace": {"enabled": "yes"}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts hook policy", "[unit][config][hooks]") {
  auto result = config::Config::parse(R"json({
  "hooks": {
    "timeout_ms": 75
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE(result->hooks().timeout_ms == 75);
}

TEST_CASE("Config::parse extracts memory recall policy", "[unit][config][memory]") {
  auto result = config::Config::parse(R"json({
  "memory": {
    "longterm": {
      "recall": {
        "enabled": true,
        "limit": 7,
        "kinds": ["project", "reference"]
      }
    }
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE(result->memory().longterm.recall.enabled);
  REQUIRE(result->memory().longterm.recall.limit == 7);
  REQUIRE(result->memory().longterm.recall.kinds == std::vector<std::string>{"project", "reference"});
}

TEST_CASE("Config::parse defaults memory recall policy when absent", "[unit][config][memory]") {
  auto result = config::Config::parse(R"json({"memory": {}})json");

  REQUIRE(result.has_value());
  REQUIRE(result->memory().longterm.recall.enabled);
  REQUIRE(result->memory().longterm.recall.limit == 20);
  REQUIRE(result->memory().longterm.recall.kinds.empty());
}

TEST_CASE("Config::parse rejects malformed memory recall policy", "[unit][config][memory]") {
  SECTION("non-object memory block") {
    auto result = config::Config::parse(R"json({"memory": []})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-object longterm block") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": []}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-object recall block") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": []}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-boolean enabled") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": {"enabled": "yes"}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("zero limit") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": {"limit": 0}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-array kinds") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": {"kinds": "project"}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty kinds") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": {"kinds": []}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty kind name") {
    auto result = config::Config::parse(R"json({"memory": {"longterm": {"recall": {"kinds": [""]}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("duplicate kind name") {
    auto result =
        config::Config::parse(R"json({"memory": {"longterm": {"recall": {"kinds": ["project", "project"]}}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse rejects malformed hook policy", "[unit][config][hooks]") {
  SECTION("non-object hooks block") {
    auto result = config::Config::parse(R"json({"hooks": []})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-integer timeout") {
    auto result = config::Config::parse(R"json({"hooks": {"timeout_ms": "slow"}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("zero timeout") {
    auto result = config::Config::parse(R"json({"hooks": {"timeout_ms": 0}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("PermissionVerdict round-trips through its stable spellings", "[unit][config]") {
  REQUIRE(core::enum_name(config::PermissionVerdict::allow) == "allow");
  REQUIRE(core::enum_name(config::PermissionVerdict::deny) == "deny");
  REQUIRE(core::enum_name(config::PermissionVerdict::ask) == "ask");

  using core::parse_enum;
  REQUIRE(parse_enum<config::PermissionVerdict>("allow") == config::PermissionVerdict::allow);
  REQUIRE(parse_enum<config::PermissionVerdict>("deny") == config::PermissionVerdict::deny);
  REQUIRE(parse_enum<config::PermissionVerdict>("ask") == config::PermissionVerdict::ask);
  REQUIRE_FALSE(parse_enum<config::PermissionVerdict>("approve").has_value());
  REQUIRE_FALSE(parse_enum<config::PermissionVerdict>("").has_value());
}

TEST_CASE("Config::parse extracts a populated permissions block", "[unit][config][permissions]") {
  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "allow": [
      {"tool_pattern": "FileRead"},
      {"tool_pattern": "*", "capability": "egress_http"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"}
    ],
    "ask": [
      {"tool_pattern": "FileWrite"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"}
    ]
  }
}
)json");

  REQUIRE(result.has_value());
  const auto& perms = result->permissions();
  REQUIRE(perms.rules.size() == 5);

  REQUIRE(perms.rules[0].verdict == config::PermissionVerdict::allow);
  REQUIRE(perms.rules[0].tool_pattern == "FileRead");
  REQUIRE_FALSE(perms.rules[0].capability.has_value());

  REQUIRE(perms.rules[1].verdict == config::PermissionVerdict::allow);
  REQUIRE(perms.rules[1].capability == core::Capability::egress_http);

  REQUIRE(perms.rules[2].verdict == config::PermissionVerdict::deny);
  REQUIRE(perms.rules[2].capability == core::Capability::runtime_loader);

  REQUIRE(perms.rules[3].verdict == config::PermissionVerdict::ask);
  REQUIRE(perms.rules[3].tool_pattern == "FileWrite");

  REQUIRE(perms.rules[4].verdict == config::PermissionVerdict::ask);
  REQUIRE(perms.rules[4].capability == core::Capability::spawn_subprocess);
}

TEST_CASE("Config::parse preserves authoring order across verdict keys", "[unit][config][permissions]") {
  // The JSON object iterates `ask` before `allow` here. The parser uses
  // object-iteration order so the operator's authoring intent survives.
  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "ask": [{"tool_pattern": "first"}],
    "allow": [{"tool_pattern": "second"}],
    "deny": [{"tool_pattern": "third"}]
  }
}
)json");

  REQUIRE(result.has_value());
  const auto& rules = result->permissions().rules;
  REQUIRE(rules.size() == 3);
  REQUIRE(rules[0].verdict == config::PermissionVerdict::ask);
  REQUIRE(rules[0].tool_pattern == "first");
  REQUIRE(rules[1].verdict == config::PermissionVerdict::allow);
  REQUIRE(rules[1].tool_pattern == "second");
  REQUIRE(rules[2].verdict == config::PermissionVerdict::deny);
  REQUIRE(rules[2].tool_pattern == "third");
}

TEST_CASE("Config::parse env-substitutes inside permission rules", "[unit][config][permissions]") {
  ScopedEnv pattern{"ORAN_CONFIG_TEST_PATTERN", "File*"};

  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "allow": [{"tool_pattern": "${ORAN_CONFIG_TEST_PATTERN}", "capability": "read_file"}]
  }
}
)json");

  REQUIRE(result.has_value());
  REQUIRE(result->permissions().rules.size() == 1);
  REQUIRE(result->permissions().rules[0].tool_pattern == "File*");
  REQUIRE(result->permissions().rules[0].capability == core::Capability::read_file);
}

TEST_CASE("Config::parse rejects malformed permission rules", "[unit][config][permissions]") {
  SECTION("missing tool_pattern") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": [{"capability": "read_file"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty tool_pattern") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": [{"tool_pattern": ""}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown capability spelling") {
    auto result = config::Config::parse(
        R"json({"permissions": {"allow": [{"tool_pattern": "*", "capability": "transcend_reality"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-object rule entry") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": ["FileRead"]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("verdict array is not an array") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": {"tool_pattern": "*"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts input_pattern on permission rules", "[unit][config][permissions][input_pattern]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "deny": [
      {"tool_pattern": "ShellExec", "input_pattern": "^rm "}
    ]
  }
})json");
  REQUIRE(result.has_value());
  const auto& rules = result->permissions().rules;
  REQUIRE(rules.size() == 1);
  REQUIRE(rules[0].input_pattern.has_value());
  REQUIRE(*rules[0].input_pattern == "^rm ");
}

TEST_CASE("Config::parse rejects malformed input_pattern at load time", "[unit][config][permissions][input_pattern]") {
  SECTION("invalid regex reports path + re2 error") {
    auto result = config::Config::parse(R"json({
  "permissions": {
    "deny": [
      {"tool_pattern": "ShellExec", "input_pattern": "[unclosed"}
    ]
  }
})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
    bool has_path = false;
    bool has_regex_error = false;
    for (const auto& [key, value] : result.error().context()) {
      if (key == "path" && value == "$.permissions.deny[0].input_pattern") {
        has_path = true;
      }
      if (key == "regex_error" && !value.empty()) {
        has_regex_error = true;
      }
    }
    REQUIRE(has_path);
    REQUIRE(has_regex_error);
  }

  SECTION("empty input_pattern is rejected") {
    auto result = config::Config::parse(R"json({
  "permissions": {"deny": [{"tool_pattern": "ShellExec", "input_pattern": ""}]}
})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-string input_pattern is rejected") {
    auto result = config::Config::parse(R"json({
  "permissions": {"deny": [{"tool_pattern": "ShellExec", "input_pattern": 42}]}
})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse handles unknown verdict, rule and unsupported root keys per mode",
          "[unit][config][permissions]") {
  SECTION("unknown verdict key warns in loose mode") {
    auto result = config::Config::parse(R"json({"permissions": {"approve": [{"tool_pattern": "*"}]}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->permissions().rules.empty());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.permissions.approve");
  }

  SECTION("unknown verdict key fails under strict_config") {
    auto result = config::Config::parse(
        R"json({"strict_config": true, "permissions": {"approve": [{"tool_pattern": "*"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown rule field warns in loose mode") {
    auto result =
        config::Config::parse(R"json({"permissions": {"allow": [{"tool_pattern": "*", "notes": "todo"}]}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->permissions().rules.size() == 1);
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.permissions.allow[0].notes");
  }

  SECTION("removed agents section warns in loose mode") {
    auto result = config::Config::parse(R"json({"agents": {"a": {"model": "claude"}}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.agents");
  }

  SECTION("removed agents section fails under strict_config") {
    auto result = config::Config::parse(R"json({"strict_config": true, "agents": {"a": {"model": "claude"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse extracts replay_max + approval_ttl_seconds on permission rules",
          "[unit][config][permissions][approval_policy]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "ask": [
      {"tool_pattern": "FileWrite", "replay_max": 2, "approval_ttl_seconds": 300}
    ]
  }
})json");
  REQUIRE(result.has_value());
  const auto& rules = result->permissions().rules;
  REQUIRE(rules.size() == 1);
  REQUIRE(rules[0].replay_max == std::optional<std::uint32_t>{2});
  REQUIRE(rules[0].approval_ttl_seconds == std::optional<std::int64_t>{300});
}

TEST_CASE("Config::parse leaves replay_max + approval_ttl_seconds unset by default",
          "[unit][config][permissions][approval_policy]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "ask": [
      {"tool_pattern": "FileWrite"}
    ]
  }
})json");
  REQUIRE(result.has_value());
  const auto& rules = result->permissions().rules;
  REQUIRE(rules.size() == 1);
  REQUIRE_FALSE(rules[0].replay_max.has_value());
  REQUIRE_FALSE(rules[0].approval_ttl_seconds.has_value());
}

TEST_CASE("Config::parse rejects negative replay_max", "[unit][config][permissions][approval_policy]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "ask": [
      {"tool_pattern": "FileWrite", "replay_max": -1}
    ]
  }
})json");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
  bool has_path = false;
  for (const auto& [key, value] : result.error().context()) {
    if (key == "path" && value == "$.permissions.ask[0].replay_max") {
      has_path = true;
    }
  }
  REQUIRE(has_path);
}

TEST_CASE("Config::parse rejects negative approval_ttl_seconds", "[unit][config][permissions][approval_policy]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "ask": [
      {"tool_pattern": "FileWrite", "approval_ttl_seconds": -60}
    ]
  }
})json");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
  bool has_path = false;
  for (const auto& [key, value] : result.error().context()) {
    if (key == "path" && value == "$.permissions.ask[0].approval_ttl_seconds") {
      has_path = true;
    }
  }
  REQUIRE(has_path);
}

TEST_CASE("Config::parse rejects non-integer replay_max", "[unit][config][permissions][approval_policy]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "ask": [
      {"tool_pattern": "FileWrite", "replay_max": "many"}
    ]
  }
})json");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::config);
}

TEST_CASE("Config::parse extracts permissions.workspace extra roots", "[unit][config][permissions][workspace]") {
  auto result = config::Config::parse(R"json({
  "permissions": {
    "workspace": {
      "extra_read_roots": ["/var/log/oran", "/srv/data"],
      "extra_write_roots": ["/var/lib/oran-out"]
    },
    "allow": [{"tool_pattern": "FileRead"}]
  }
})json");

  REQUIRE(result.has_value());
  const auto& workspace = result->permissions().workspace;
  REQUIRE(workspace.extra_read_roots == std::vector<std::string>{"/var/log/oran", "/srv/data"});
  REQUIRE(workspace.extra_write_roots == std::vector<std::string>{"/var/lib/oran-out"});
  // Rule parsing still works alongside the workspace block.
  REQUIRE(result->permissions().rules.size() == 1);
  REQUIRE(result->permissions().rules[0].tool_pattern == "FileRead");
}

TEST_CASE("Config::parse env-substitutes workspace roots", "[unit][config][permissions][workspace]") {
  ScopedEnv extra{"ORAN_CONFIG_TEST_EXTRA", "/srv/canonical"};

  auto result = config::Config::parse(R"json({
  "permissions": {
    "workspace": {
      "extra_read_roots": ["${ORAN_CONFIG_TEST_EXTRA}"]
    }
  }
})json");

  REQUIRE(result.has_value());
  REQUIRE(result->permissions().workspace.extra_read_roots == std::vector<std::string>{"/srv/canonical"});
  REQUIRE(result->permissions().workspace.extra_write_roots.empty());
}

TEST_CASE("Config::parse rejects malformed workspace entries", "[unit][config][permissions][workspace]") {
  SECTION("non-array extra_read_roots") {
    auto result = config::Config::parse(R"json({"permissions": {"workspace": {"extra_read_roots": "/srv/data"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-string entry") {
    auto result = config::Config::parse(R"json({"permissions": {"workspace": {"extra_write_roots": [42]}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-object workspace block") {
    auto result = config::Config::parse(R"json({"permissions": {"workspace": "/srv/data"}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse warns or fails on unknown workspace fields", "[unit][config][permissions][workspace]") {
  auto loose = config::Config::parse(R"json({"permissions": {"workspace": {"sandbox_root": "/tmp/sandbox"}}})json");
  REQUIRE(loose.has_value());
  REQUIRE(loose->warnings().size() == 1);
  REQUIRE(loose->warnings()[0].path == "$.permissions.workspace.sandbox_root");

  auto strict = config::Config::parse(
      R"json({"strict_config": true, "permissions": {"workspace": {"sandbox_root": "/tmp/sandbox"}}})json");
  REQUIRE_FALSE(strict.has_value());
  REQUIRE(strict.error().kind() == core::ErrorKind::config);
}

TEST_CASE("prompt recall rejects limits beyond the memory tool boundary", "[unit][config][memory][core_boundary]") {
  const auto result = config::Config::parse(R"({"memory":{"longterm":{"recall":{"limit":21}}}})");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind() == core::ErrorKind::config);
}

TEST_CASE("Config::parse reports unknown fields in every runtime object", "[unit][config]") {
  auto loose = config::Config::parse(R"json({"runtime": {"tool_output": {"max_text_bytes": 1, "extra": 2}}})json");
  REQUIRE(loose.has_value());
  REQUIRE(loose->warnings().size() == 1);
  CHECK(loose->warnings()[0].path == "$.runtime.tool_output.extra");
  CHECK(loose->runtime().tool_output.max_text_bytes == 1);

  auto strict = config::Config::parse(R"json({"strict_config": true, "runtime": {"stream": {"limit": 1}}})json");
  REQUIRE_FALSE(strict.has_value());
  CHECK(strict.error().kind() == core::ErrorKind::config);
}

TEST_CASE("Config::parse bounds integers before narrowing", "[unit][config]") {
  auto result = config::Config::parse(R"json({
  "profiles": {
    "default": {
      "provider": "local",
      "model": "m",
      "base_url": "http://127.0.0.1:8080",
      "api_key_env": "LOCAL_API_KEY",
      "thinking_budget": 4294967296
    }
  }
})json");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind() == core::ErrorKind::config);
}
