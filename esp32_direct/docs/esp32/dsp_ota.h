/*=============================================================================
 * dsp_ota.h -- flash a TMS320F280039C over its SCI-B from an ESP32
 *
 * Architecture:  ESP32  <--UART-->  DSP SCI-B (GPIO22 TX / GPIO23 RX)
 *                (no ARM/GD32 in the path any more)
 *
 * The DSP runs the resident bootloader described in ../README.md.  This module
 * speaks that bootloader's IAP frame protocol so the ESP32 can update the App
 * with no PC and no JTAG probe.
 *
 * The image you hand to dsp_ota_flash() is the PACKED image produced by
 *     py host/pack_f28003x.py app.out app.bin --version 0x00010000
 * i.e. the App code with the 8-word image header (magic/length/CRC32/version)
 * already injected.  Send its bytes verbatim: the tool already emits the byte
 * order the DSP expects (high byte of each word first).
 *
 * IMPORTANT: the UART is shared with whatever else talks to the DSP (your
 * direct_link task).  You must have EXCLUSIVE use of it for the whole update:
 * suspend the other task and/or take your UART mutex, then flush the RX buffer
 * before starting.  While the DSP is in its bootloader it does NOT speak the
 * 03/04/25 application protocol, and it will ignore those frames.
 *===========================================================================*/
#ifndef DSP_OTA_H
#define DSP_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DSP_OTA_OK = 0,          /* image sent, DSP is writing the last blocks and will jump */
    DSP_OTA_ERR_ARG,         /* bad arguments / image too small */
    DSP_OTA_ERR_NO_HR,       /* no "HR": DSP not in the bootloader, wrong port or baud */
    DSP_OTA_ERR_ERASE,       /* no/short erase acknowledgement */
    DSP_OTA_ERR_NO_CR,       /* erase never completed (no "CR") */
    DSP_OTA_ERR_PACKET,      /* a data packet was not acknowledged after all retries */
} dsp_ota_result_t;

/* Formatting/log sink.  `fmt` is a printf-style format string.  May be NULL. */
typedef void (*dsp_ota_log_fn)(const char *fmt, ...);

/* Send CMD_UPGRADE and wait for "HR".  Writes NOTHING to flash: use it to find
 * out whether the DSP is sitting in its bootloader and listening.
 * Returns true if "HR" was received. */
bool dsp_ota_probe(int uart_num, int timeout_ms, int retries, dsp_ota_log_fn log);

/* Full update: handshake -> erase App area (sectors 2..47) -> 1024-byte data
 * packets -> EOT.  The bootloader sectors 0/1 are never touched.
 * `bin`/`len` must be the packed image (with header). */
dsp_ota_result_t dsp_ota_flash(int uart_num, const uint8_t *bin, size_t len,
                               dsp_ota_log_fn log);

/* Optional: callback invoked after each acknowledged packet, for a progress bar.
 * `sent` and `total` are byte counts. */
typedef void (*dsp_ota_progress_fn)(size_t sent, size_t total);
void dsp_ota_set_progress(dsp_ota_progress_fn cb);

#endif /* DSP_OTA_H */
