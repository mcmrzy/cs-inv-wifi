#ifndef OTA_SIGNATURE_H
#define OTA_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

bool ota_verify_release_signature(const char* target, const char* version,
                                  uint32_t size, const char* sha256,
                                  uint32_t security_version,
                                  const char* signature_base64);

#endif
