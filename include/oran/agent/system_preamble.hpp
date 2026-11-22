#pragma once

#include <string>

namespace orangutan::agent {

struct SystemPreamble {
  std::string section_text;

  friend bool operator==(const SystemPreamble&, const SystemPreamble&) = default;
};

[[nodiscard]] SystemPreamble default_system_preamble();

}  // namespace orangutan::agent
