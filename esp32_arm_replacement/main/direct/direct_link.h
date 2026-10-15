#ifndef DIRECT_LINK_H
#define DIRECT_LINK_H
#include "direct_protocol.h"
#include "direct_dsp_stream.h"
#include "direct_dsp_params.h"
#include "arm_param.h"
#include "direct_bms_identity.h"
#include "direct_iap.h"
/* Synchronous transfer, dispatched to the existing target UART owner. Source
 * callbacks/context must stay valid until return. Only read_image, optional
 * policy/cancel callbacks and progress are used from caller_io; UART callbacks
 * are supplied by this bridge. Progress runs in the calling task. Concurrent
 * transfers, uninitialized target tasks and pending DSP writes are rejected.
 * Do not call from a DSP/BMS UART owner or while holding the snapshot mutex. */
direct_iap_result_t direct_link_run_iap(const direct_iap_image_t *image,
                                      const direct_iap_io_t *caller_io);
typedef enum {DIRECT_DSP_ENTER_BOOT=1,DIRECT_DSP_EXIT_BOOT=2,DIRECT_DSP_REBOOT=3} direct_dsp_control_t;
/* Submits CMD42/HR or EOT to the existing UART owner. ENTER_BOOT holds
 * normal DSP polling until EXIT_BOOT, REBOOT or a successful flash job.
 * Failed/uncertain entry also holds polling; caller must explicitly recover. */
direct_iap_result_t direct_link_dsp_control(direct_dsp_control_t operation);
bool direct_dsp_boot_active(void);
bool direct_config_get(uint16_t index, uint16_t *value);
/* Cloud/BLE wire units, unlike the native DSP values used by the LCD. */
bool direct_config_get_wire(uint16_t index,uint16_t *value);
/* One coherent cloud snapshot, including known-field mask and saved revision.
 * Acquires the shared mutex; call only without holding the parameter lock. */
bool direct_config_snapshot_wire(uint16_t values[CTRL_PARAM_COUNT],
                                 bool valid[CTRL_PARAM_COUNT],uint32_t *revision);
void direct_config_snapshot(uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                            bool valid[DIRECT_DSP_CONFIG_COUNT]);
bool direct_config_writable(uint16_t index);
/* Queues one write; 0=accepted, -1=busy/offline/unknown/unsupported. */
int direct_config_write(uint16_t index, uint16_t value);
/* Reset only controller alarm/blank/buzzer preferences, retaining DSP power settings. */
int direct_config_reset_local(void);
/* UI result: 1=pending, 2=saved on ESP and sync frames submitted,
 * 0=legacy/local confirmed, negative=failed. UART submission is not DSP ACK. */
int direct_config_last_write_result(void);
bool direct_dsp_get_status(direct_dsp_status_t *status);
bool direct_dsp_get_status_sample(direct_dsp_status_t *status,uint32_t *received_ms);
/* Only command04 refreshes this timestamp. Returns false after 2 seconds. */
bool direct_dsp_get_measurement(RunParamDef *measurement,uint32_t *received_ms);
typedef struct {
    direct_dsp_rx_stats_t rx;
    uint32_t tx_frames, tx_errors, rx_errors, cmd03, cmd04, cmd25;
    uint32_t last_tx_ms,max_tx_gap_ms,tx_gap_over_500_ms,poll_lock_misses;
    /* 1=quiet, 2=IAP, 4=Boot, 8=invalid params, 16=unsaved params,
     * 32=import, 64=write awaiting durable save. */
    uint8_t poll_hold_reason;
    uint32_t sync_requests, rejected_frames, unhandled_frames;
    uint32_t sync_replies, sync_tx_errors;
    uint32_t sync_collision_hints;
    uint8_t sync_pending_mask;
    bool sync_params_ready, sync_params_durable;
    uint32_t last_byte_ms, last_frame_ms, last_status_ms, last_measurement_ms;
    uint32_t stack_min_free_bytes; /* ESP-IDF watermark unit is bytes. */
    uint32_t uart_fifo_overflows, uart_buffer_full, uart_frame_errors;
    uint32_t uart_parity_errors, uart_breaks, uart_queue_full_observations;
    direct_dsp_rx_capture_t fault, measurement_frame;
    uint8_t last_command, last_length, sync_command;
    uint8_t prefix[24], prefix_length;
    bool task_running, quiet;
} direct_dsp_diagnostics_t;
/* Counters survive temporary loss; timestamps are uptime milliseconds.
 * Snapshot under the shared mutex, never consume UART from USB/MQTT tasks. */
bool direct_dsp_get_diagnostics(direct_dsp_diagnostics_t *diagnostics);
/* Authorized local import; atomic complete bundle bound to current SN.
 * Return 0=saved, -1=busy, -2=invalid, -3=storage failed. */
int direct_dsp_params_install(const uint8_t *bundle,size_t length,const char *sn);
bool direct_dsp_params_export(direct_dsp_params_t *params,bool *durable);
bool direct_bms_get_snapshot(direct_bms_measurement_t *measurement,
                            uint32_t *age_ms);
/* Driver/tasks alive; does not assert that an external DSP/BMS replied. */
bool direct_link_software_healthy(void);
/* Explicit optional read-only scan; executes in the BMS UART owner, never USB
 * task. A completed empty/partial mask is not upgrade authorization. */
bool direct_bms_identity_request(void);
bool direct_bms_identity_snapshot(direct_bms_identity_t *identity,bool *pending,
                                  uint8_t *queried,uint32_t *age_ms);
#endif
