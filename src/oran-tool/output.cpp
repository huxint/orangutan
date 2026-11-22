// src/oran-tool/output.cpp — structured output helpers.

#include <oran/tool/output.hpp>

#include <cstddef>

#include <oran/core/str.hpp>

namespace orangutan::tool {

OutputCapReport apply_output_caps(Output& output, OutputCapOptions options) {
  auto report = OutputCapReport{
      .text_bytes_before = output.text.size(),
      .text_bytes_after = output.text.size(),
      .data_bytes_before = output.data_json.has_value() ? output.data_json->size() : std::size_t{0},
  };

  if (options.max_text_bytes != 0 && output.text.size() > options.max_text_bytes) {
    const auto capped = core::str::truncate_to_code_point(output.text, options.max_text_bytes);
    output.text.resize(capped.size());
    output.usage.truncated = true;
    report.text_truncated = true;
    report.text_bytes_after = output.text.size();
  }

  if (options.max_data_bytes != 0 && output.data_json.has_value() &&
      output.data_json->size() > options.max_data_bytes) {
    output.data_json.reset();
    output.usage.data_dropped = true;
    report.data_dropped = true;
  }

  return report;
}

}  // namespace orangutan::tool
