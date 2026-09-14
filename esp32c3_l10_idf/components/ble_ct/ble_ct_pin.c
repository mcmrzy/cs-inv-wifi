#include "ble_ct_pin.h"
#include "ble_ct_clock.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ble_ct_pin";

/* PRODUCT_SECRET 分片拼接 */
static const char S1[] = "CS_INV_L10";
static const char S2[] = "_2026_";
static const char S3[] = "SECRET";

#define MAX_FAIL_COUNT 5
#define LOCKOUT_DURATION_S (30 * 60)

typedef struct {
    uint8_t addr[6];
    int fail_count;
    uint32_t lockout_until;
} PinLockout;

#define MAX_LOCKOUT_ENTRIES 4
static PinLockout s_lockouts[MAX_LOCKOUT_ENTRIES];

static void get_secret(uint8_t *buf, size_t *len) {
    size_t n = 0;
    memcpy(buf + n, S1, strlen(S1)); n += strlen(S1);
    memcpy(buf + n, S2, strlen(S2)); n += strlen(S2);
    memcpy(buf + n, S3, strlen(S3)); n += strlen(S3);
    *len = n;
}

uint32_t ble_ct_compute_pin(const char *sn) {
    uint8_t secret[64];
    size_t secret_len;
    get_secret(secret, &secret_len);

    uint8_t digest[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, secret, secret_len,
                    (const uint8_t *)sn, strlen(sn), digest);

    return ((digest[0] << 16) | (digest[1] << 8) | digest[2]) % 1000000;
}

static PinLockout *find_lockout(const uint8_t *addr) {
    int empty = -1;
    for (int i = 0; i < MAX_LOCKOUT_ENTRIES; i++) {
        if (memcmp(s_lockouts[i].addr, addr, 6) == 0) return &s_lockouts[i];
        if (empty < 0 && s_lockouts[i].fail_count == 0) empty = i;
    }
    if (empty >= 0) {
        memset(&s_lockouts[empty], 0, sizeof(PinLockout));
        memcpy(s_lockouts[empty].addr, addr, 6);
        return &s_lockouts[empty];
    }
    return NULL;
}

bool ble_ct_pin_verify(const char *sn, const char *pin, const uint8_t *peer_addr) {
    PinLockout *entry = find_lockout(peer_addr);
    if (entry && entry->fail_count >= MAX_FAIL_COUNT) {
        uint32_t now = ble_ct_now();
        if (now < entry->lockout_until) {
            ESP_LOGW(TAG, "PIN locked out for %us", entry->lockout_until - now);
            return false;
        }
        entry->fail_count = 0;
    }

    uint32_t expected = ble_ct_compute_pin(sn);
    char expected_str[8];
    snprintf(expected_str, sizeof(expected_str), "%06lu", (unsigned long)expected);

    ESP_LOGI(TAG, "verify: sn=%s pin=%s expected=%s", sn, pin, expected_str);

    if (strcmp(pin, expected_str) == 0) {
        if (entry) entry->fail_count = 0;
        ESP_LOGI(TAG, "PIN verified OK");
        return true;
    }

    if (entry) {
        entry->fail_count++;
        if (entry->fail_count >= MAX_FAIL_COUNT) {
            entry->lockout_until = ble_ct_now() + LOCKOUT_DURATION_S;
            ESP_LOGW(TAG, "PIN lockout: %d failures", entry->fail_count);
        }
    }
    ESP_LOGW(TAG, "PIN mismatch");
    return false;
}

uint32_t ble_ct_pin_lockout_remaining(const uint8_t *peer_addr) {
    PinLockout *entry = find_lockout(peer_addr);
    if (!entry || entry->fail_count < MAX_FAIL_COUNT) return 0;
    uint32_t now = ble_ct_now();
    if (now >= entry->lockout_until) { entry->fail_count = 0; return 0; }
    return entry->lockout_until - now;
}
