#pragma once

#include <cstdint>
#include <string>

namespace orangutan::core {

/// Derived session context; never an authorization source.
struct WorkingContext {
  std::int64_t covered_sequence{};
  std::int64_t revision{};
  std::string summary;
  friend bool operator==(const WorkingContext&, const WorkingContext&) = default;
};

}  // namespace orangutan::core
