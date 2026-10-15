#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef enum {DIRECT_IAP_DSP=2,DIRECT_IAP_BMS=3} direct_iap_target_t;
typedef enum {DIRECT_IAP_OK,DIRECT_IAP_INCOMPATIBLE,DIRECT_IAP_READ_FAILED,DIRECT_IAP_TX_FAILED,DIRECT_IAP_ACK_FAILED,DIRECT_IAP_TARGET_ERROR,DIRECT_IAP_CANCELLED,DIRECT_IAP_VERSION_UNCONFIRMED,DIRECT_IAP_NO_MEMORY,
 DIRECT_IAP_IMAGE_SIZE_INVALID,DIRECT_IAP_IMAGE_OPCODE_INVALID,DIRECT_IAP_IMAGE_MAGIC_INVALID,
 DIRECT_IAP_BUSY,DIRECT_IAP_NOT_READY} direct_iap_result_t;

static inline const char *direct_iap_result_name(direct_iap_result_t result){
 switch(result){
 case DIRECT_IAP_OK:return "OK";
 case DIRECT_IAP_INCOMPATIBLE:return "INVALID_IO_OR_TARGET";
 case DIRECT_IAP_READ_FAILED:return "IMAGE_READ_FAILED";
 case DIRECT_IAP_TX_FAILED:return "UART_TX_FAILED";
 case DIRECT_IAP_ACK_FAILED:return "DSP_OR_BMS_ACK_TIMEOUT";
 case DIRECT_IAP_TARGET_ERROR:return "TARGET_ERROR";
 case DIRECT_IAP_CANCELLED:return "CANCELLED";
 case DIRECT_IAP_VERSION_UNCONFIRMED:return "VERSION_UNCONFIRMED";
 case DIRECT_IAP_NO_MEMORY:return "NO_MEMORY";
 case DIRECT_IAP_IMAGE_SIZE_INVALID:return "IMAGE_SIZE_INVALID";
 case DIRECT_IAP_IMAGE_OPCODE_INVALID:return "IMAGE_LB_OPCODE_INVALID";
 case DIRECT_IAP_IMAGE_MAGIC_INVALID:return "IMAGE_MAGIC_INVALID";
 case DIRECT_IAP_BUSY:return "UART_JOB_BUSY";
 case DIRECT_IAP_NOT_READY:return "UART_OWNER_NOT_READY";
 default:return "UNKNOWN";
 }
}
typedef struct {direct_iap_target_t target;size_t size;uint8_t hw_type;uint16_t hw_model,customer;char version[32];} direct_iap_image_t;
typedef enum {DIRECT_IAP_PHASE_NONE,DIRECT_IAP_PHASE_HR,
 DIRECT_IAP_PHASE_ERASE_ACK,DIRECT_IAP_PHASE_ERASE_CR,
 DIRECT_IAP_PHASE_DATA_ACK,DIRECT_IAP_PHASE_EOT} direct_iap_phase_t;
/* Owner records numbers/bytes only. The consumer formats the snapshot. */
#define DIRECT_IAP_RX_HEAD_SIZE 24U
typedef struct {
 direct_iap_phase_t phase;
 unsigned tx_frames,tx_len,tx_max_ms,rx_bytes;
 uint8_t rx_head[DIRECT_IAP_RX_HEAD_SIZE],rx_head_len;
} direct_iap_trace_t;
/* Called ONLY by a target UART transaction owner. verify_identity must confirm
 * actual target identity + stopped maintenance condition before any Boot command.
 * verify_version must independently read the restarted application version.
 * Absent callbacks/wildcard identity never authorize destructive operations. */
typedef struct {
 bool (*read_image)(void*,size_t,uint8_t*,size_t);
 bool (*send)(void*,const uint8_t*,size_t);
 size_t (*receive)(void*,uint8_t*,size_t,unsigned timeout_ms);
 void (*delay)(void*,unsigned ms);
 bool (*verify_identity)(void*,const direct_iap_image_t*);
 bool (*verify_version)(void*,const direct_iap_image_t*);
 bool (*cancelled)(void*);
 void (*flush)(void*);
 void (*progress)(void*,size_t,size_t);
 uint32_t (*now_ms)(void*); /* Monotonic uptime for bounded DSP OTA deadlines. */
 void *context;
 void (*phase)(void*,direct_iap_phase_t); /* Optional diagnostic; no logging. */
} direct_iap_io_t;
direct_iap_result_t direct_iap_run(const direct_iap_image_t*,const direct_iap_io_t*);

/* Port of the device-validated ARM forwarding path. size is the RAW file
 * length; this core appends zero bytes up to an 8-byte boundary exactly as
 * esp32c3_l10_idf arm_ota did. It sends neither the Collector's 16-byte zero
 * header nor TEA ciphertext to a target. The read callback is never asked
 * for padding bytes. BMS handshake uses the old all-zero identity wildcard.
 * verify_identity / verify_version are not called; OK means target protocol
 * completion (DSP CRs+EOT, BMS FC+CRC ACK), not independent version readback.
 * DSP address is the original u16 modulo arithmetic, including wrap above
 * 256KiB. Accepted transport size does not establish target flash capacity.
 * Caller must own the UART and validate staged image/hash and target choice. */
direct_iap_result_t direct_iap_run_legacy(const direct_iap_image_t*,
                                        const direct_iap_io_t*);
