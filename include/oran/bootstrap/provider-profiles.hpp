#pragma once

#include <string_view>

#include <oran/core/result.hpp>
#include <oran/provider/protocol_transport.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

/// Map configured routes, aliases and model policy into owned provider values.
/// Provider construction validates transport support before credential lookup.
[[nodiscard]] core::Result<provider::RouteProfileResolution>
resolve_route_profiles(const config::Config& config, std::string_view route_name = "default");

}  // namespace orangutan::bootstrap
