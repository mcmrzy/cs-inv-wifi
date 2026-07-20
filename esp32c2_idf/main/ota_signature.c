#include "ota_signature.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "monocypher-ed25519.h"

#include "ota_release_pubkey.h"

#define ED25519_SIGNATURE_SIZE 64U
#define ED25519_BASE64_SIZE    88U

static bool valid_target(const char *target)
{
    return target != NULL &&
           (strcmp(target, "esp") == 0 || strcmp(target, "arm") == 0 ||
            strcmp(target, "dsp") == 0 || strcmp(target, "bms") == 0);
}

static bool valid_version(const char *version)
{
    if (version == NULL) {
        return false;
    }

    const size_t length = strlen(version);
    if (length == 0 || length > 31) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        const char c = version[i];
        const bool allowed = (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') ||
                             c == '.' || c == '_' || c == '+' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool valid_sha256(const char *sha256)
{
    if (sha256 == NULL || strlen(sha256) != 64) {
        return false;
    }

    for (size_t i = 0; i < 64; ++i) {
        const char c = sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool valid_base64_signature(const char *signature_base64)
{
    if (signature_base64 == NULL || strlen(signature_base64) != ED25519_BASE64_SIZE ||
        signature_base64[86] != '=' || signature_base64[87] != '=') {
        return false;
    }

    for (size_t i = 0; i < 86; ++i) {
        const char c = signature_base64[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            return false;
        }
    }
    return true;
}

bool ota_verify_release_signature(const char *target, const char *version,
                                  uint32_t size, const char *sha256,
                                  uint32_t security_version,
                                  const char *signature_base64)
{
    if (!valid_target(target) || !valid_version(version) || size == 0 ||
        !valid_sha256(sha256) || security_version == 0 ||
        !valid_base64_signature(signature_base64)) {
        return false;
    }

    uint8_t signature[ED25519_SIGNATURE_SIZE];
    size_t signature_len = 0;
    const int decode_result = mbedtls_base64_decode(
        signature, sizeof(signature), &signature_len,
        (const unsigned char *)signature_base64, ED25519_BASE64_SIZE);
    if (decode_result != 0 || signature_len != 64) {
        crypto_wipe(signature, sizeof(signature));
        return false;
    }

    char message[256];
    const int message_len = snprintf(
        message, sizeof(message),
        "CS_INV_OTA_V1\ntarget=%s\nversion=%s\nsize=%lu\nsha256=%s\n"
        "security_version=%lu\n",
        target, version, (unsigned long)size, sha256,
        (unsigned long)security_version);
    if (message_len <= 0 || message_len >= (int)sizeof(message)) {
        crypto_wipe(signature, sizeof(signature));
        crypto_wipe(message, sizeof(message));
        return false;
    }

    const bool verified = crypto_ed25519_check(
        signature, OTA_RELEASE_PUBLIC_KEY,
        (const uint8_t *)message, (size_t)message_len) == 0;
    crypto_wipe(signature, sizeof(signature));
    crypto_wipe(message, sizeof(message));
    return verified;
}
