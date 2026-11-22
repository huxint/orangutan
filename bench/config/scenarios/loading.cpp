#include <nanobench.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <oran/config.hpp>

namespace orangutan::bench {
namespace config = orangutan::config;

namespace {

constexpr auto kConfigText = std::string_view{R"json({
  "runtime": {
    "workers": 4,
    "request_timeout_ms": 600000,
    "prompt": {
      "active_tools": [
        "FileRead",
        "FileEdit",
        "MemoryRemember"
      ]
    }
  },
  "profiles": {
    "default": {
      "provider": "anthropic",
      "model": "claude-3-5-sonnet-latest",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": []
    }
  }
})json"};

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
  std::abort();
}

[[gnu::noinline]] std::size_t parse_memory() {
  auto parsed = config::Config::parse(kConfigText);
  if (!parsed) {
    std::abort();
  }
  return parsed->profiles().size() + parsed->routes().size() + parsed->runtime().prompt.active_tools.tool_names.size();
}

[[gnu::noinline]] std::size_t load_example_file() {
  static const auto path = example_config_path();
  auto parsed = config::Config::load_file(path);
  if (!parsed) {
    std::abort();
  }
  return parsed->profiles().size() + parsed->routes().size() + parsed->runtime().prompt.active_tools.tool_names.size();
}

}  // namespace

void register_loading(ankerl::nanobench::Bench& bench) {
  bench.run("config.parse_memory", [] {
    const auto value = parse_memory();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("config.load_file_example", [] {
    const auto value = load_example_file();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
