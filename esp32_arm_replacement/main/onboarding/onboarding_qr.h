#ifndef ONBOARDING_QR_H
#define ONBOARDING_QR_H
#include <stdbool.h>
#include <stdint.h>
#define ONBOARDING_QR_BYTES 212
#define ONBOARDING_PAYLOAD_BYTES 64
typedef struct {
    uint8_t modules[ONBOARDING_QR_BYTES];
    char payload[ONBOARDING_PAYLOAD_BYTES];
    bool ready;
    uint32_t generation;
} onboarding_qr_t;
bool onboarding_qr_update(onboarding_qr_t *,const char *sn,const char *pin);
int onboarding_qr_size(const onboarding_qr_t *);
bool onboarding_qr_module(const onboarding_qr_t *,int x,int y);
#endif
