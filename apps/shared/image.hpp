#pragma once
#include <oran/core/content.hpp>
#include <oran/core/result.hpp>
#include <sodium/utils.h>
#include <string_view>

namespace orangutan::chat_host {
inline core::Result<core::ImageContent> image_content(std::string_view bytes, std::size_t image_max_bytes) {
  if (bytes.size() > image_max_bytes)
    return std::unexpected(core::Error::invalid_argument("image exceeds 5 MiB"));
  std::string media;
  if (bytes.starts_with("\x89PNG\r\n\x1a\n"))
    media = "image/png";
  else if (bytes.starts_with("\xff\xd8\xff"))
    media = "image/jpeg";
  else if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a"))
    media = "image/gif";
  else if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP")
    media = "image/webp";
  else
    return std::unexpected(core::Error::invalid_argument("unsupported image format"));
  std::string base64(sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(base64.data(),
                    base64.size(),
                    reinterpret_cast<const unsigned char*>(bytes.data()),
                    bytes.size(),
                    sodium_base64_VARIANT_ORIGINAL);
  base64.pop_back();  // The library includes the terminating NUL in its encoded length.
  return core::ImageContent{std::move(media), std::move(base64)};
}
}  // namespace orangutan::chat_host
