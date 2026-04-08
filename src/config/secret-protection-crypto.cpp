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

        constexpr std::string_view PROTECTED_PREFIX = "enc:v1:";
        constexpr unsigned int PBKDF2_ITERATIONS = 200000;
        constexpr std::size_t SALT_SIZE = 16;
        constexpr std::size_t IV_SIZE = 12;
        constexpr std::size_t TAG_SIZE = 16;
        constexpr std::size_t KEY_SIZE = 32;
        constexpr std::string_view BASE64URL_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        constexpr int INVALID_BASE64URL_CHAR = -1;
        constexpr std::string_view RNG_PERSONALIZATION = "orangutan-config-secret";

        template <std::size_t Size>
        using byte_array = std::array<std::byte, Size>;

        using byte_vector = std::vector<std::byte>;
        using const_byte_span = std::span<const std::byte>;
        using mutable_byte_span = std::span<std::byte>;
        using decode_table = std::array<int, 256>;

        class EntropyContext final {
          public:
            EntropyContext() {
                mbedtls_entropy_init(&value_);
            }

            ~EntropyContext() {
                mbedtls_entropy_free(&value_);
            }

            EntropyContext(const EntropyContext &) = delete;
            EntropyContext &operator=(const EntropyContext &) = delete;
            EntropyContext(EntropyContext &&) = delete;
            EntropyContext &operator=(EntropyContext &&) = delete;

            [[nodiscard]]
            mbedtls_entropy_context *get() noexcept {
                return &value_;
            }

          private:
            mbedtls_entropy_context value_{};
        };

        class CtrDrbgContext final {
          public:
            CtrDrbgContext() {
                mbedtls_ctr_drbg_init(&value_);
            }

            ~CtrDrbgContext() {
                mbedtls_ctr_drbg_free(&value_);
            }

            CtrDrbgContext(const CtrDrbgContext &) = delete;
            CtrDrbgContext &operator=(const CtrDrbgContext &) = delete;
            CtrDrbgContext(CtrDrbgContext &&) = delete;
            CtrDrbgContext &operator=(CtrDrbgContext &&) = delete;

            [[nodiscard]]
            mbedtls_ctr_drbg_context *get() noexcept {
                return &value_;
            }

          private:
            mbedtls_ctr_drbg_context value_{};
        };

        class MdContext final {
          public:
            MdContext() {
                mbedtls_md_init(&value_);
            }

            ~MdContext() {
                mbedtls_md_free(&value_);
            }

            MdContext(const MdContext &) = delete;
            MdContext &operator=(const MdContext &) = delete;
            MdContext(MdContext &&) = delete;
            MdContext &operator=(MdContext &&) = delete;

            [[nodiscard]]
            mbedtls_md_context_t *get() noexcept {
                return &value_;
            }

          private:
            mbedtls_md_context_t value_{};
        };

        class GcmContext final {
          public:
            GcmContext() {
                mbedtls_gcm_init(&value_);
            }

            ~GcmContext() {
                mbedtls_gcm_free(&value_);
            }

            GcmContext(const GcmContext &) = delete;
            GcmContext &operator=(const GcmContext &) = delete;
            GcmContext(GcmContext &&) = delete;
            GcmContext &operator=(GcmContext &&) = delete;

            [[nodiscard]]
            mbedtls_gcm_context *get() noexcept {
                return &value_;
            }

          private:
            mbedtls_gcm_context value_{};
        };


        struct ProtectedPayload {
            byte_array<SALT_SIZE> salt{};
            byte_array<IV_SIZE> iv{};
            byte_array<TAG_SIZE> tag{};
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
            table.fill(INVALID_BASE64URL_CHAR);

            for (std::size_t index = 0; index < BASE64URL_ALPHABET.size(); ++index) {
                table.at(static_cast<std::size_t>(to_unsigned_char(BASE64URL_ALPHABET.at(index)))) = static_cast<int>(index);
            }
            return table;
        }

        constexpr auto BASE64URL_DECODE_TABLE = make_base64url_decode_table();

        [[nodiscard]]
        constexpr bool is_base64url_char(char ch) noexcept {
            return BASE64URL_DECODE_TABLE.at(static_cast<std::size_t>(to_unsigned_char(ch))) != INVALID_BASE64URL_CHAR;
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
                encoded.push_back(BASE64URL_ALPHABET[(combined >> 18U) & 0x3FU]);
                encoded.push_back(BASE64URL_ALPHABET[(combined >> 12U) & 0x3FU]);
                if (chunk_size >= 2) {
                    encoded.push_back(BASE64URL_ALPHABET[(combined >> 6U) & 0x3FU]);
                }
                if (chunk_size == 3) {
                    encoded.push_back(BASE64URL_ALPHABET[combined & 0x3FU]);
                }
            }

            return encoded;
        }

        [[nodiscard]]
        constexpr int decode_base64url_char(char ch) noexcept {
            const auto value = BASE64URL_DECODE_TABLE.at(static_cast<std::size_t>(to_unsigned_char(ch)));
            if (value != INVALID_BASE64URL_CHAR) {
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
            EntropyContext entropy;
            CtrDrbgContext ctr_drbg;

            if (mbedtls_ctr_drbg_seed(ctr_drbg.get(), mbedtls_entropy_func, entropy.get(), mbedtls_const_chars(RNG_PERSONALIZATION), RNG_PERSONALIZATION.size()) != 0) {
                throw ConfigSecretProtectionError("Failed to generate config secret protection randomness.");
            }

            if (mbedtls_ctr_drbg_random(ctr_drbg.get(), mbedtls_mutable_bytes(output), output.size()) != 0) {
                throw ConfigSecretProtectionError("Failed to generate config secret protection randomness.");
            }
        }

        [[nodiscard]]
        byte_array<KEY_SIZE> derive_key(std::string_view password, std::span<const std::byte, SALT_SIZE> salt) {
            const auto *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            if (md_info == nullptr) {
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            MdContext md_ctx;
            if (mbedtls_md_setup(md_ctx.get(), md_info, 1) != 0) {
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            byte_array<KEY_SIZE> key{};
            if (mbedtls_pkcs5_pbkdf2_hmac(md_ctx.get(), mbedtls_const_chars(password), password.size(), mbedtls_const_bytes(salt), salt.size(), PBKDF2_ITERATIONS, key.size(),
                                          mbedtls_mutable_bytes(std::span{key})) != 0) {
                throw ConfigSecretProtectionError("Failed to derive config secret protection key.");
            }

            return key;
        }


        [[nodiscard]]
        std::string aad_for_field(std::string_view field_kind) {
            return "orangutan-config-secret:v1:" + std::string(field_kind);
        }

        [[nodiscard]]
        byte_vector encrypt_aes_gcm(const_byte_span plaintext, std::span<const std::byte, KEY_SIZE> key, std::span<const std::byte, IV_SIZE> iv, const_byte_span aad,
                                    byte_array<TAG_SIZE> &tag) {
            GcmContext ctx;
            if (mbedtls_gcm_setkey(ctx.get(), MBEDTLS_CIPHER_ID_AES, mbedtls_const_bytes(key), static_cast<unsigned int>(key.size() * 8U)) != 0) {
                throw ConfigSecretProtectionError("Failed to initialize config secret encryption.");
            }

            byte_vector ciphertext(plaintext.size());
            if (mbedtls_gcm_crypt_and_tag(ctx.get(), MBEDTLS_GCM_ENCRYPT, plaintext.size(), mbedtls_const_bytes(iv), iv.size(), mbedtls_const_bytes(aad), aad.size(),
                                          mbedtls_const_bytes(plaintext), mbedtls_mutable_bytes(std::span{ciphertext}), tag.size(), mbedtls_mutable_bytes(std::span{tag})) != 0) {
                throw ConfigSecretProtectionError("Failed to encrypt config secret.");
            }

            return ciphertext;
        }


        [[nodiscard]]
        byte_vector decrypt_aes_gcm(const_byte_span ciphertext, std::span<const std::byte, KEY_SIZE> key, std::span<const std::byte, IV_SIZE> iv,
                                    std::span<const std::byte, TAG_SIZE> tag, const_byte_span aad, std::string_view display_field) {
            GcmContext ctx;
            if (mbedtls_gcm_setkey(ctx.get(), MBEDTLS_CIPHER_ID_AES, mbedtls_const_bytes(key), static_cast<unsigned int>(key.size() * 8U)) != 0) {
                throw ConfigSecretProtectionError("Failed to initialize config secret decryption for '" + std::string(display_field) + "'.");
            }

            byte_vector plaintext(ciphertext.size());
            if (mbedtls_gcm_auth_decrypt(ctx.get(), ciphertext.size(), mbedtls_const_bytes(iv), iv.size(), mbedtls_const_bytes(aad), aad.size(), mbedtls_const_bytes(tag), tag.size(),
                                         mbedtls_const_bytes(ciphertext), mbedtls_mutable_bytes(std::span{plaintext})) != 0) {
                throw ConfigSecretProtectionError("Failed to decrypt protected config secret for '" + std::string(display_field) + "'.");
            }

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
            const auto decoded = decode_base64url(stored_value.substr(PROTECTED_PREFIX.size()));
            if (decoded.size() < SALT_SIZE + IV_SIZE + TAG_SIZE) {
                throw ConfigSecretProtectionError("Protected config secret payload is malformed for '" + std::string(display_field) + "'.");
            }

            auto remaining = const_byte_span(decoded);
            ProtectedPayload payload;

            std::ranges::copy(remaining.first<SALT_SIZE>(), payload.salt.begin());
            remaining = remaining.subspan(SALT_SIZE);

            std::ranges::copy(remaining.first<IV_SIZE>(), payload.iv.begin());
            remaining = remaining.subspan(IV_SIZE);

            std::ranges::copy(remaining.first<TAG_SIZE>(), payload.tag.begin());
            remaining = remaining.subspan(TAG_SIZE);

            payload.ciphertext.assign(remaining.begin(), remaining.end());
            return payload;
        }

    } // namespace

    bool is_protected_config_secret(std::string_view value) {
        return value.starts_with(PROTECTED_PREFIX);
    }

    std::string protect_config_secret(std::string_view plaintext, std::string_view password, std::string_view field_kind) {
        if (password.empty()) {
            throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
        }

        byte_array<SALT_SIZE> salt{};
        byte_array<IV_SIZE> iv{};
        fill_random_bytes(std::span{salt});
        fill_random_bytes(std::span{iv});

        const auto key = derive_key(password, salt);
        byte_array<TAG_SIZE> tag{};
        const auto aad = aad_for_field(field_kind);
        auto ciphertext = encrypt_aes_gcm(bytes_from_string_view(plaintext), key, iv, bytes_from_string_view(aad), tag);

        const ProtectedPayload payload{
            .salt = salt,
            .iv = iv,
            .tag = tag,
            .ciphertext = std::move(ciphertext),
        };
        return std::string(PROTECTED_PREFIX) + encode_base64url(serialize_payload(payload));
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
