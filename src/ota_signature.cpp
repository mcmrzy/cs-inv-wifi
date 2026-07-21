#include "ota_signature.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "ota_release_pubkey.h"

static bool valid_target(const char* target) {
    return target &&
           (strcmp(target, "esp") == 0 || strcmp(target, "arm") == 0 ||
            strcmp(target, "dsp") == 0 || strcmp(target, "bms") == 0);
}

static bool valid_version(const char* version) {
    if (!version) return false;
    size_t len = strlen(version);
    if (len == 0 || len > 31) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = version[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '+' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_sha256(const char* sha256) {
    if (!sha256 || strlen(sha256) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool ota_verify_release_signature(const char* target, const char* version,
                                  uint32_t size, const char* sha256,
                                  uint32_t security_version,
                                  const char* signature_base64) {
    if (!valid_target(target) || !valid_version(version) || size == 0 ||
        !valid_sha256(sha256) || security_version == 0 || !signature_base64 ||
        strlen(signature_base64) != 88 ||
        signature_base64[86] != '=' || signature_base64[87] != '=') {
        return false;
    }
    if (sodium_init() < 0) return false;

    uint8_t signature[crypto_sign_BYTES];
    size_t signature_len = 0;
    if (sodium_base642bin(signature, sizeof(signature), signature_base64, 88,
                         NULL, &signature_len, NULL,
                         sodium_base64_VARIANT_ORIGINAL) != 0 ||
        signature_len != crypto_sign_BYTES) {
        sodium_memzero(signature, sizeof(signature));
        return false;
    }

    char message[256];
    int message_len = snprintf(
        message, sizeof(message),
        "CS_INV_OTA_V1\ntarget=%s\nversion=%s\nsize=%lu\nsha256=%s\n"
        "security_version=%lu\n",
        target, version, (unsigned long)size, sha256,
        (unsigned long)security_version);
    bool valid = message_len > 0 && message_len < (int)sizeof(message) &&
                 crypto_sign_verify_detached(
                     signature, (const unsigned char*)message,
                     (unsigned long long)message_len,
                     OTA_RELEASE_PUBLIC_KEY) == 0;
    sodium_memzero(signature, sizeof(signature));
    sodium_memzero(message, sizeof(message));
    return valid;
}
