// include/oran/provider/protocol_request.hpp - provider protocol request bytes.
//
// This is the offline serialization step before HTTP transport. It turns the
// provider-domain request and one selected model target into a vendor protocol
// path plus JSON body bytes. Authentication headers, retries, streaming I/O,
// and response decoding belong to later adapter/transport slices.

#pragma once

#include <string>

#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider {

/// Serialized request ready for a future HTTP client.
///
/// `body_json` is a UTF-8 JSON object. Public headers intentionally expose
/// bytes, not a JSON parser type, to keep heavy dependencies in `.cpp` files.
struct ProtocolRequest {
  std::string method{"POST"};
  std::string path;
  std::string body_json;

  friend bool operator==(const ProtocolRequest&, const ProtocolRequest&) = default;
};

/// Serialize a request for the selected protocol target.
///
/// Currently supports `anthropic_messages` and `openai_responses`. Unsupported
/// protocols return `ErrorKind::config`; malformed opaque JSON fields such as
/// tool schemas, tool inputs, or structured tool results return
/// `ErrorKind::parsing` with non-secret context.
[[nodiscard]] core::Result<ProtocolRequest> make_protocol_request(const Request& request, const ModelTarget& target);

}  // namespace orangutan::provider
