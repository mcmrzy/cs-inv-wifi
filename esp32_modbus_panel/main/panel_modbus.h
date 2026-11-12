#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_MB_MAX 256U
typedef struct { uint8_t data[PANEL_MB_MAX]; size_t used; } panel_mb_stream_t;
typedef struct {
    uint8_t function, exception;
    uint16_t start, count, values[125];
} panel_mb_reply_t;
uint16_t panel_mb_crc(const uint8_t *data, size_t length);
/* Bounded serial diagnostic, at most 24 bytes; does not validate a frame. */
void panel_mb_hex_head(const uint8_t *,size_t length,char *out,size_t capacity);
size_t panel_mb_read(uint8_t address, uint16_t start, uint16_t count, uint8_t out[8]);
size_t panel_mb_read_input(uint8_t address, uint16_t start, uint16_t count, uint8_t out[8]);
size_t panel_mb_write(uint8_t address, uint16_t reg, uint16_t value, uint8_t out[11]);
/* Returns a complete CRC-valid response matching this transaction, including
 * exception replies. One stream belongs to one UART transaction. A new request
 * resets stream.used and drains the UART before sending. */
bool panel_mb_feed(panel_mb_stream_t *stream, uint8_t byte, uint8_t address,
                   uint8_t function, uint16_t start, uint16_t count,
                   panel_mb_reply_t *reply);
