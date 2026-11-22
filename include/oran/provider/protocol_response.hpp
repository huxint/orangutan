// include/oran/provider/protocol_response.hpp - provider protocol response bytes.
//
// This is the offline decoding step before HTTP transport. It turns a vendor
// protocol JSON response body plus one selected model target into the
// provider-domain Response shape. Transport status handling, streaming event
// assembly, retries, and authentication belong to later adapter slices.

#pragma once

#include <string_view>

#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider {

/// Decode a vendor response body for the selected protocol target.
///
/// Currently supports `anthropic_messages` and `openai_responses`. Unsupported
/// protocols return `ErrorKind::config`; malformed or unsupported response
/// shapes return `ErrorKind::parsing` with non-secret context.
[[nodiscard]] core::Result<Response> decode_protocol_response(std::string_view body_json, const ModelTarget& target);

}  // namespace orangutan::provider
