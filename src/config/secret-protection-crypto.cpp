#include "config/secret-protection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace orangutan::config {
    namespace {

        constexpr std::string_view protected_prefix = "enc:v1:";
        constexpr unsigned int pbkdf2_iterations = 200000;
        constexpr std::size_t salt_size = 16;
        constexpr std::size_t iv_size = 12;
        constexpr std::size_t tag_size = 16;
        constexpr std::size_t key_size = 32;
        constexpr std::string_view base64url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        constexpr int invalid_base64url_char = -1;
        constexpr std::string_view rng_personalization = "orangutan-config-secret";

        template <std::size_t Size>
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

            for (std::size_t index = 0; index < base64url_alphabet.size(); ++index) {
                table.at(static_cast<std::size_t>(to_unsigned_char(base64url_alphabet.at(index)))) = static_cast<int>(index);
            }
            return table;
        }

        constexpr auto base64url_decode_table = make_base64url_decode_table();

        [[nodiscard]]
        constexpr bool is_base64url_char(char ch) noexcept {
            return base64url_decode_table.at(static_cast<std::size_t>(to_unsigned_char(ch))) != invalid_base64url_char;
        }

        [[nodiscard]]
        const unsigned char *mbedtls_const_bytes(const_byte_span bytes) noexcept {
            return reinterpret_cast<const unsigned char *>(bytes.data());
        }

        [[nodiscard]]
        unsigned char *mbedtls_mutable_bytes(mutable_byte_span bytes) noexcept {
            return reinterpret_cast<unsigned char *>(bytes.data());
        }

        [[nodiscard]]
        const unsigned char *mbedtls_const_chars(std::string_view text) noexcept {
            return reinterpret_cast<const unsigned char *>(text.data());
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
                const auto chunk_size = static_cast<std::size_t>(std::distance(bytes.begin(), copy_result.out));

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
            const auto value = base64url_decode_table.at(static_cast<std::size_t>(to_unsigned_char(ch)));
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

        void fill_random_bytes(mutable_byte_span output) {
            mbedtls_entropy_context entropy;
            mbedtls_ctr_drbg_context ctr_drbg;
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&ctr_drbg);

            const auto cleanup = [&] {
                mbedtls_ctr_drbg_free(&ctr_drbg);
                mbedtls_entropy_free(&entropy);
            };

            if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, mbedtls_const_chars(rng_personalization), rng_personalization.size()) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to generate config secret protection randomness.");
            }

            if (mbedtls_ctr_drbg_random(&ctr_drbg, mbedtls_mutable_bytes(output), output.size()) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to generate config secret protection randomness.");
            }

            cleanup();
        }

        [[nodiscard]]
        byte_array<key_size> derive_key(std::string_view password, std::span<const std::byte, salt_size> salt) {
            const auto *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            if (md_info == nullptr) {
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            mbedtls_md_context_t md_ctx;
            mbedtls_md_init(&md_ctx);

            const auto cleanup = [&] {
                mbedtls_md_free(&md_ctx);
            };

            if (mbedtls_md_setup(&md_ctx, md_info, 1) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            byte_array<key_size> key{};
            if (mbedtls_pkcs5_pbkdf2_hmac(&md_ctx, mbedtls_const_chars(password), password.size(), mbedtls_const_bytes(salt), salt.size(), pbkdf2_iterations, key.size(),
                                          mbedtls_mutable_bytes(std::span{key})) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            cleanup();
            return key;
        }

        [[nodiscard]]
        std::string aad_for_field(std::string_view field_kind) {
            return "orangutan-config-secret:v1:" + std::string(field_kind);
        }

        [[nodiscard]]
        byte_vector encrypt_aes_gcm(const_byte_span plaintext, std::span<const std::byte, key_size> key, std::span<const std::byte, iv_size> iv, const_byte_span aad,
                                    byte_array<tag_size> &tag) {
            mbedtls_gcm_context ctx;
            mbedtls_gcm_init(&ctx);

            const auto cleanup = [&] {
                mbedtls_gcm_free(&ctx);
            };

            if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, mbedtls_const_bytes(key), static_cast<unsigned int>(key.size() * 8U)) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to initialize config secret encryption.");
            }

            byte_vector ciphertext(plaintext.size());
            if (mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plaintext.size(), mbedtls_const_bytes(iv), iv.size(), mbedtls_const_bytes(aad), aad.size(),
                                          mbedtls_const_bytes(plaintext), mbedtls_mutable_bytes(std::span{ciphertext}), tag.size(), mbedtls_mutable_bytes(std::span{tag})) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to encrypt config secret.");
            }

            cleanup();
            return ciphertext;
        }

        [[nodiscard]]
        byte_vector decrypt_aes_gcm(const_byte_span ciphertext, std::span<const std::byte, key_size> key, std::span<const std::byte, iv_size> iv,
                                    std::span<const std::byte, tag_size> tag, const_byte_span aad, std::string_view display_field) {
            mbedtls_gcm_context ctx;
            mbedtls_gcm_init(&ctx);

            const auto cleanup = [&] {
                mbedtls_gcm_free(&ctx);
            };

            if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, mbedtls_const_bytes(key), static_cast<unsigned int>(key.size() * 8U)) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to initialize config secret decryption for '" + std::string(display_field) + "'.");
            }

            byte_vector plaintext(ciphertext.size());
            if (mbedtls_gcm_auth_decrypt(&ctx, ciphertext.size(), mbedtls_const_bytes(iv), iv.size(), mbedtls_const_bytes(aad), aad.size(), mbedtls_const_bytes(tag), tag.size(),
                                         mbedtls_const_bytes(ciphertext), mbedtls_mutable_bytes(std::span{plaintext})) != 0) {
                cleanup();
                throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
            }

            cleanup();
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
        fill_random_bytes(std::span{salt});
        fill_random_bytes(std::span{iv});

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

} // namespace orangutan::config
