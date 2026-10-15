#ifndef DIRECT_DSP_OTA_TRANSPORT_H
#define DIRECT_DSP_OTA_TRANSPORT_H
#include "direct_iap.h"

#define DIRECT_DSP_OTA_PACKET_SIZE 1024U
#define DIRECT_DSP_OTA_MAX_PACKETS 367U
#define DIRECT_DSP_OTA_MAX_SIZE (DIRECT_DSP_OTA_PACKET_SIZE * DIRECT_DSP_OTA_MAX_PACKETS)

/* UART-owner-only wire protocol. CMD42 asks the DSP App to enter its Boot.
 * A successful probe leaves Boot active: do not resume application polling
 * until a flash job or explicit EOT exit. Never use this as a passive query. */
direct_iap_result_t direct_dsp_ota_enter_boot(const direct_iap_io_t *io);
direct_iap_result_t direct_dsp_ota_exit_boot(const direct_iap_io_t *io);

/* Matches the supplied ESP module's preflight: >=64 bytes, even length,
 * big-endian LB opcode and magic high half 0x5A5A. Full CRC32/header length
 * layout is not in the supplied files; do not invent or claim that check.
 * Service separately verifies the staged file's SHA256 when supplied. */
direct_iap_result_t direct_dsp_ota_validate(size_t image_size,const direct_iap_io_t *io);
/* Streams the supplied packed image verbatim, validating BEFORE CMD42.
 * max size follows supplied protocol's conservative 367-packet limit.
 * Packets are up to 1024B; the supplied module sends a short final packet.
 * OK means all packet ACKs and EOT were sent, not App/version confirmation.
 * On failure, leave Boot active; never send EOT for an incomplete transfer. */
direct_iap_result_t direct_dsp_ota_flash(size_t image_size,const direct_iap_io_t *io);
#endif
