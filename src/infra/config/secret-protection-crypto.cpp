#include "infra/config/secret-protection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace orangutan {
namespace {

constexpr std::string_view protected_prefix = "enc:v1:";
constexpr int pbkdf2_iterations = 200000;
constexpr size_t salt_size = 16;
constexpr size_t iv_size = 12;
constexpr size_t tag_size = 16;
constexpr size_t key_size = 32;
constexpr std::string_view base64url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr int invalid_base64url_char = -1;

template <size_t Size>
using byte_array = std::array<std::byte, Size>;

using byte_vector = std::vector<std::byte>;
using const_byte_span = std::span<const std::byte>;
using mutable_byte_span = std::span<std::byte>;
using decode_table = std::array<int, 256>;

struct ProtectedPayload {
    byte_array<salt_size> salt{};
    byte_array<iv_size> iv{};
    byte_array<tag_size> tag{};
    byte_vector ciphertext;
};

[[nodiscard]]
constexpr unsigned char to_unsigned_char(char ch) noexcept {
    return static_cast<unsigned char>(ch);
}

[[nodiscard]]
constexpr std::byte to_byte(char ch) noexcept {
    return static_cast<std::byte>(to_unsigned_char(ch));
}

[[nodiscard]]
constexpr decode_table make_base64url_decode_table() noexcept {
    decode_table table{};
    table.fill(invalid_base64url_char);

    for (size_t index = 0; index < base64url_alphabet.size(); ++index) {
        table.at(static_cast<size_t>(to_unsigned_char(base64url_alphabet.at(index)))) = static_cast<int>(index);
    }
    return table;
}

constexpr auto base64url_decode_table = make_base64url_decode_table();

[[nodiscard]]
constexpr bool is_base64url_char(char ch) noexcept {
    return base64url_decode_table.at(static_cast<size_t>(to_unsigned_char(ch))) != invalid_base64url_char;
}

[[nodiscard]]
const unsigned char *openssl_const_bytes(const_byte_span bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const unsigned char *>(bytes.data());
}

[[nodiscard]]
unsigned char *openssl_mutable_bytes(mutable_byte_span bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<unsigned char *>(bytes.data());
}

[[nodiscard]]
std::string string_from_bytes(const_byte_span bytes) {
    std::string text;
    text.reserve(bytes.size());
    std::ranges::transform(bytes, std::back_inserter(text), [](std::byte value) {
        return static_cast<char>(std::to_integer<unsigned char>(value));
    });
    return text;
}

[[nodiscard]]
byte_vector bytes_from_string_view(std::string_view input) {
    byte_vector bytes;
    bytes.reserve(input.size());
    std::ranges::transform(input, std::back_inserter(bytes), to_byte);
    return bytes;
}

[[nodiscard]]
std::string encode_base64url(const_byte_span data) {
    std::string encoded;
    encoded.reserve(((data.size() * 4) + 2) / 3);

    for (const auto chunk : data | std::views::chunk(3)) {
        byte_array<3> bytes{};
        const auto copy_result = std::ranges::copy(chunk, bytes.begin());
        const auto chunk_size = static_cast<size_t>(std::distance(bytes.begin(), copy_result.out));

        const auto combined = (std::to_integer<unsigned int>(bytes[0]) << 16U) | (std::to_integer<unsigned int>(bytes[1]) << 8U) | std::to_integer<unsigned int>(bytes[2]);
        encoded.push_back(base64url_alphabet[(combined >> 18U) & 0x3FU]);
        encoded.push_back(base64url_alphabet[(combined >> 12U) & 0x3FU]);
        if (chunk_size >= 2) {
            encoded.push_back(base64url_alphabet[(combined >> 6U) & 0x3FU]);
        }
        if (chunk_size == 3) {
            encoded.push_back(base64url_alphabet[combined & 0x3FU]);
        }
    }

    return encoded;
}

[[nodiscard]]
constexpr int decode_base64url_char(char ch) noexcept {
    const auto value = base64url_decode_table.at(static_cast<size_t>(to_unsigned_char(ch)));
    if (value != invalid_base64url_char) {
        return value;
    }
    std::unreachable();
}

[[nodiscard]]
byte_vector decode_base64url(std::string_view input) {
    if (input.empty()) {
        return {};
    }

    byte_vector decoded;
    decoded.reserve((input.size() * 3) / 4);

    unsigned int accumulator = 0;
    int bits_collected = 0;
    for (const auto ch : input) {
        if (!is_base64url_char(ch)) {
            throw ConfigSecretProtectionError("Protected config secret payload is malformed.");
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned int>(decode_base64url_char(ch));
        bits_collected += 6;
        if (bits_collected >= 8) {
            bits_collected -= 8;
            decoded.push_back(static_cast<std::byte>((accumulator >> bits_collected) & 0xFFU));
        }
    }

    if (bits_collected == 1) {
        throw ConfigSecretProtectionError("Protected config secret payload is malformed.");
    }

    return decoded;
}

[[nodiscard]]
byte_array<key_size> derive_key(std::string_view password, std::span<const std::byte, salt_size> salt) {
    byte_array<key_size> key{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), openssl_const_bytes(salt), static_cast<int>(salt.size()), pbkdf2_iterations, EVP_sha256(),
                          static_cast<int>(key.size()), openssl_mutable_bytes(std::span{key})) != 1) {
        throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
    }
    return key;
}

[[nodiscard]]
std::string aad_for_field(std::string_view field_kind) {
    return "orangutan-config-secret:v1:" + std::string(field_kind);
}

[[nodiscard]]
byte_vector encrypt_aes_gcm(const_byte_span plaintext, std::span<const std::byte, key_size> key, std::span<const std::byte, iv_size> iv, const_byte_span aad,
                            byte_array<tag_size> &tag) {
    byte_vector ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);

    using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) {
        throw ConfigSecretProtectionError("Failed to initialize config secret encryption.");
    }
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, openssl_const_bytes(key), openssl_const_bytes(iv)) != 1) {
        throw ConfigSecretProtectionError("Failed to initialize config secret encryption.");
    }

    int written = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &written, openssl_const_bytes(aad), static_cast<int>(aad.size())) != 1) {
        throw ConfigSecretProtectionError("Failed to initialize config secret encryption.");
    }

    int ciphertext_len = 0;
    if (!plaintext.empty() &&
        EVP_EncryptUpdate(ctx.get(), openssl_mutable_bytes(std::span{ciphertext}), &written, openssl_const_bytes(plaintext), static_cast<int>(plaintext.size())) != 1) {
        throw ConfigSecretProtectionError("Failed to encrypt config secret.");
    }
    ciphertext_len += written;

    if (EVP_EncryptFinal_ex(ctx.get(), openssl_mutable_bytes(std::span{ciphertext}.subspan(static_cast<size_t>(ciphertext_len))), &written) != 1) {
        throw ConfigSecretProtectionError("Failed to encrypt config secret.");
    }
    ciphertext_len += written;

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), openssl_mutable_bytes(std::span{tag})) != 1) {
        throw ConfigSecretProtectionError("Failed to finalize config secret encryption.");
    }

    ciphertext.resize(static_cast<size_t>(ciphertext_len));
    return ciphertext;
}

[[nodiscard]]
byte_vector decrypt_aes_gcm(const_byte_span ciphertext, std::span<const std::byte, key_size> key, std::span<const std::byte, iv_size> iv, std::span<const std::byte, tag_size> tag,
                            const_byte_span aad, std::string_view display_field) {
    byte_vector plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);

    using ctx_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    ctx_ptr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) {
        throw ConfigSecretProtectionError("Failed to initialize config secret decryption for '" + std::string(display_field) + "'.");
    }
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, openssl_const_bytes(key), openssl_const_bytes(iv)) != 1) {
        throw ConfigSecretProtectionError("Failed to initialize config secret decryption for '" + std::string(display_field) + "'.");
    }

    int written = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &written, openssl_const_bytes(aad), static_cast<int>(aad.size())) != 1) {
        throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
    }

    int plaintext_len = 0;
    if (!ciphertext.empty() &&
        EVP_DecryptUpdate(ctx.get(), openssl_mutable_bytes(std::span{plaintext}), &written, openssl_const_bytes(ciphertext), static_cast<int>(ciphertext.size())) != 1) {
        throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
    }
    plaintext_len += written;

    byte_array<tag_size> mutable_tag{};
    std::ranges::copy(tag, mutable_tag.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(mutable_tag.size()), openssl_mutable_bytes(std::span{mutable_tag})) != 1) {
        throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
    }

    if (EVP_DecryptFinal_ex(ctx.get(), openssl_mutable_bytes(std::span{plaintext}.subspan(static_cast<size_t>(plaintext_len))), &written) != 1) {
        throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
    }
    plaintext_len += written;

    plaintext.resize(static_cast<size_t>(plaintext_len));
    return plaintext;
}

[[nodiscard]]
byte_vector serialize_payload(const ProtectedPayload &payload) {
    byte_vector serialized;
    serialized.reserve(payload.salt.size() + payload.iv.size() + payload.tag.size() + payload.ciphertext.size());
    serialized.insert(serialized.end(), payload.salt.begin(), payload.salt.end());
    serialized.insert(serialized.end(), payload.iv.begin(), payload.iv.end());
    serialized.insert(serialized.end(), payload.tag.begin(), payload.tag.end());
    serialized.insert(serialized.end(), payload.ciphertext.begin(), payload.ciphertext.end());
    return serialized;
}

[[nodiscard]]
ProtectedPayload deserialize_payload(std::string_view stored_value, std::string_view display_field) {
    const auto decoded = decode_base64url(stored_value.substr(protected_prefix.size()));
    if (decoded.size() < salt_size + iv_size + tag_size) {
        throw ConfigSecretProtectionError("Protected config secret payload is malformed for '" + std::string(display_field) + "'.");
    }

    auto remaining = const_byte_span(decoded);
    ProtectedPayload payload;

    std::ranges::copy(remaining.first<salt_size>(), payload.salt.begin());
    remaining = remaining.subspan(salt_size);

    std::ranges::copy(remaining.first<iv_size>(), payload.iv.begin());
    remaining = remaining.subspan(iv_size);

    std::ranges::copy(remaining.first<tag_size>(), payload.tag.begin());
    remaining = remaining.subspan(tag_size);

    payload.ciphertext.assign(remaining.begin(), remaining.end());
    return payload;
}

} // namespace

bool is_protected_config_secret(std::string_view value) {
    return value.starts_with(protected_prefix);
}

std::string protect_config_secret(std::string_view plaintext, std::string_view password, std::string_view field_kind) {
    if (password.empty()) {
        throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
    }

    byte_array<salt_size> salt{};
    byte_array<iv_size> iv{};
    if (RAND_bytes(openssl_mutable_bytes(std::span{salt}), static_cast<int>(salt.size())) != 1 ||
        RAND_bytes(openssl_mutable_bytes(std::span{iv}), static_cast<int>(iv.size())) != 1) {
        throw ConfigSecretProtectionError("Failed to generate config secret protection randomness.");
    }

    const auto key = derive_key(password, salt);
    byte_array<tag_size> tag{};
    const auto aad = aad_for_field(field_kind);
    auto ciphertext = encrypt_aes_gcm(bytes_from_string_view(plaintext), key, iv, bytes_from_string_view(aad), tag);

    const ProtectedPayload payload{
        .salt = salt,
        .iv = iv,
        .tag = tag,
        .ciphertext = std::move(ciphertext),
    };
    return std::string(protected_prefix) + encode_base64url(serialize_payload(payload));
}

std::string reveal_config_secret(std::string_view stored_value, std::string_view password, std::string_view field_kind, std::string_view display_field) {
    if (!is_protected_config_secret(stored_value)) {
        return std::string(stored_value);
    }
    if (password.empty()) {
        throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
    }

    const auto payload = deserialize_payload(stored_value, display_field);
    const auto key = derive_key(password, payload.salt);
    const auto aad = aad_for_field(field_kind);
    const auto plaintext = decrypt_aes_gcm(payload.ciphertext, key, payload.iv, payload.tag, bytes_from_string_view(aad), display_field);

    return string_from_bytes(plaintext);
}

} // namespace orangutan
