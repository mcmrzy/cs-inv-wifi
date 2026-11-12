#include "panel_modbus.h"
#include <stdio.h>

void panel_mb_hex_head(const uint8_t *data,size_t length,char *out,size_t capacity)
{
    if(!out || !capacity)return;
    out[0]=0;
    if(!data || !length){snprintf(out,capacity,"-");return;}
    if(length>24)length=24;
    size_t used=0;
    for(size_t i=0;i<length && capacity-used>=4;i++){
        int n=snprintf(out+used,capacity-used,"%s%02X",i?" ":"",data[i]);
        if(n<=0 || (size_t)n>=capacity-used)break;
        used+=(size_t)n;
    }
}
#include <string.h>

uint16_t panel_mb_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    if (!data && length) return 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1U) ? 0xa001U : 0U));
    }
    return crc;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static size_t finish(uint8_t *out, size_t bytes)
{
    uint16_t crc = panel_mb_crc(out, bytes);
    out[bytes] = (uint8_t)crc;
    out[bytes + 1] = (uint8_t)(crc >> 8);
    return bytes + 2;
}

static size_t read_registers(uint8_t address, uint8_t function, uint16_t start,
                             uint16_t count, uint8_t out[8])
{
    if (!out || !address || address > 247 || !count || count > 125 ||
        (uint32_t)start + count > 65536U) return 0;
    out[0] = address;
    out[1] = function;
    put16(out + 2, start);
    put16(out + 4, count);
    return finish(out, 6);
}

size_t panel_mb_read(uint8_t address, uint16_t start, uint16_t count, uint8_t out[8])
{
    return read_registers(address, 3, start, count, out);
}

size_t panel_mb_read_input(uint8_t address, uint16_t start, uint16_t count, uint8_t out[8])
{
    return read_registers(address, 4, start, count, out);
}

size_t panel_mb_write(uint8_t address, uint16_t reg, uint16_t value, uint8_t out[11])
{
    if (!out || !address || address > 247) return 0;
    out[0] = address;
    out[1] = 0x10;
    put16(out + 2, reg);
    put16(out + 4, 1);
    out[6] = 2;
    put16(out + 7, value);
    return finish(out, 9);
}

bool panel_mb_feed(panel_mb_stream_t *stream, uint8_t byte, uint8_t address,
                   uint8_t function, uint16_t start, uint16_t count,
                   panel_mb_reply_t *reply)
{
    if (!stream || !reply || !address || address > 247 ||
        (function != 3 && function != 4 && function != 0x10) || !count || count > 125) return false;
    if (stream->used >= sizeof stream->data) {
        memmove(stream->data, stream->data + 1, sizeof stream->data - 1);
        stream->used = sizeof stream->data - 1;
    }
    stream->data[stream->used++] = byte;
    /* Search all candidates, so an incomplete bogus length cannot hide a valid
     * later frame. Exact expected byte count rejects oversized/corrupt headers. */
    for (size_t offset = 0; offset + 2 <= stream->used; ++offset) {
        const uint8_t *p = stream->data + offset;
        const size_t available = stream->used - offset;
        if (p[0] != address) continue;
        bool exception = p[1] == (uint8_t)(function | 0x80U);
        if (!exception && p[1] != function) continue;
        size_t length;
        if (exception) length = 5;
        else if (function == 0x10) length = 8;
        else {
            if (available < 3 || p[2] != count * 2U) continue;
            length = 5U + p[2];
        }
        if (available < length || panel_mb_crc(p, length) != 0) continue;
        if (!exception && function == 0x10 &&
            (get16(p + 2) != start || get16(p + 4) != count)) continue;
        if (exception && (p[2] == 0 || p[2] > 0x0b)) continue;
        memset(reply, 0, sizeof *reply);
        reply->function = function;
        reply->start = start;
        if (exception) reply->exception = p[2];
        else {
            reply->count = count;
            if (function != 0x10)
                for (unsigned i = 0; i < count; ++i)
                    reply->values[i] = get16(p + 3 + i * 2U);
        }
        size_t consumed = offset + length;
        memmove(stream->data, stream->data + consumed, stream->used - consumed);
        stream->used -= consumed;
        return true;
    }
    return false;
}
