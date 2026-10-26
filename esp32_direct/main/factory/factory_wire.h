#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define FACTORY_PAYLOAD_MAX 4096
#define FACTORY_FRAME_MAX (FACTORY_PAYLOAD_MAX+14)
typedef struct {uint8_t command;uint32_t id;uint16_t length;uint8_t payload[FACTORY_PAYLOAD_MAX];} factory_frame_t;
typedef struct {uint8_t command;uint32_t id;uint16_t length;const uint8_t *payload;} factory_frame_view_t;
typedef struct {size_t used;uint8_t bytes[FACTORY_FRAME_MAX];} factory_parser_t;
uint32_t factory_wire_crc32(const uint8_t*,size_t);
size_t factory_frame_encode(const factory_frame_t*,uint8_t*,size_t);
/* Payload may overlap the destination, including its header or payload area. */
size_t factory_frame_view_encode(const factory_frame_view_t*,uint8_t*,size_t);
/* A successful view borrows parser storage until the next feed/encoding call.
 * Consume the request fully before reusing that storage for the response. */
bool factory_parser_feed_view(factory_parser_t*,uint8_t,factory_frame_view_t*);
bool factory_parser_feed(factory_parser_t*,uint8_t,factory_frame_t*);
