#include "direct_protocol.h"

#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

bool direct_dsp_parse_cmd03(const uint8_t *frame, size_t len,
                           direct_dsp_status_t *out)
{
    if (!out || len < 22U || !direct_dsp_validate(frame, len) || frame[2] != 3)
        return false;
    direct_dsp_status_t status = {0};
    status.fault = ((uint32_t)frame[3] << 24) | ((uint32_t)frame[4] << 16) |
                   ((uint32_t)frame[5] << 8) | frame[6];
    for (unsigned i = 0; i < 3; ++i) {
        status.inverter_status[i] = be16(frame + 7 + i * 2);
        status.warning[i] = be16(frame + 13 + i * 2);
    }
    status.mode = frame[19];
    *out = status;
    return true;
}

bool direct_dsp_parse_cmd25(const uint8_t *frame, size_t len,
                            uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                            bool valid[DIRECT_DSP_CONFIG_COUNT])
{
    /* App(10) reads through byte40; require these bytes before the CRC. */
    if (!values || !valid || len < 43U || !direct_dsp_validate(frame, len) ||
        frame[2] != 0x25) return false;
    /* These are active-profile observations, not the absent ARM's stored
     * inactive profiles. Switching type/material must not preserve validity. */
    for (unsigned i=67;i<=81;++i) valid[i]=false;
    static const uint8_t bytes[][2] = {
        {0,3},{13,4},{2,5},{1,11},{4,12},{40,23},{36,24},{37,25},
        {10,26},{26,27},{30,28},{32,35},{33,36},{31,39},{48,40}
    };
    static const uint8_t words[][2] = {
        {7,7},{8,9},{11,19},{12,21},{27,29},{28,31},{29,33}
    };
    for (unsigned i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i) {
        values[bytes[i][0]] = frame[bytes[i][1]];
        valid[bytes[i][0]] = true;
    }
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        values[words[i][0]] = be16(frame + words[i][1]);
        valid[words[i][0]] = true;
    }
    /* Profile material for USER2 is ARM-owned and unknown; never guess it. */
    uint8_t bulk = 0, floating = 0, cutoff = 0;
    switch (frame[12]) {
        case 1: bulk=67; floating=70; cutoff=76; break;
        case 2: bulk=68; floating=71; cutoff=77; break;
        case 3: bulk=69; floating=73; cutoff=79; break;
        case 4: floating=72; cutoff=78; break;
        default: break;
    }
    if (bulk) { values[bulk]=be16(frame+13); valid[bulk]=true; }
    if (floating) { values[floating]=be16(frame+15); valid[floating]=true; }
    if (cutoff) { values[cutoff]=be16(frame+17); valid[cutoff]=true; }
    return true;
}

bool direct_dsp_config_writable(uint16_t index)
{
    switch (index) {
        case 0: case 1: case 4: case 7: case 8: case 10: case 11: case 12:
        case 13: case 36: case 37: case 40: case 48:
        case 67: case 68: case 69: case 70: case 71: case 72: case 73:
        case 76: case 77: case 78: case 79: return true;
        default: return false;
    }
}

size_t direct_dsp_make_charge_write(uint16_t bulk, uint16_t floating,
                                    uint16_t cutoff, uint8_t out[11])
{
    if (!out || bulk<400 || bulk>650 || floating<400 || floating>650 ||
        cutoff<400 || cutoff>650 || cutoff>=floating || floating>bulk) return 0;
    out[0]=1; out[1]=7; out[2]=0x2f;
    uint16_t values[3]={bulk,floating,cutoff};
    for (unsigned i=0;i<3;++i) {
        out[3+i*2]=(uint8_t)(values[i]>>8); out[4+i*2]=(uint8_t)values[i];
    }
    const uint16_t crc=direct_crc16(out,9);
    out[9]=(uint8_t)crc; out[10]=(uint8_t)(crc>>8); return 11;
}

size_t direct_dsp_make_config_write(uint16_t index, uint16_t value,
                          const uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                          const bool valid[DIRECT_DSP_CONFIG_COUNT],
                          uint8_t out[16])
{
    if (!values || !valid || !out || !direct_dsp_config_writable(index) ||
        index>=67 || !valid[index]) return 0;
    uint16_t v[DIRECT_DSP_CONFIG_COUNT];
    memcpy(v, values, sizeof(v)); v[index] = value;
    memset(out, 0, 16); out[0]=1;
    size_t payload=0;
    if (index == 7 || index == 8) {
        if ((index == 7 && (value < 2000 || value > 2500)) ||
            (index == 8 && value != 5000 && value != 6000)) return 0;
        out[2]=(uint8_t)index; payload=2;
        out[3]=(uint8_t)(value>>8); out[4]=(uint8_t)value;
    } else if (index == 4 || index == 48) {
        if (!valid[4] || !valid[48] || v[4] < 1 || v[4] > 5 || v[48]>255)
            return 0;
        out[2]=0x15; payload=2; out[3]=(uint8_t)v[4];
        out[4]=v[4]==4 ? (uint8_t)v[48] : 0;
        /* Protocol cannot be changed in a non-lithium battery mode. */
        if (index == 48 && v[4] != 4) return 0;
    } else if (index == 0 || index == 13) {
        if (!valid[0] || !valid[13] || v[0]<1 || v[0]>4 ||
            (v[13]!=1 && v[13]!=3 && v[13]!=4)) return 0;
        out[2]=0x18; payload=2; out[3]=(uint8_t)v[0]; out[4]=(uint8_t)v[13];
    } else {
        uint16_t indexes[3]; size_t count;
        if (index == 1 || index == 10) {
            indexes[0]=1; indexes[1]=10; count=2; out[2]=0x17;
            if (v[1]>200 || v[10]>200 || v[10]>v[1]) return 0;
        } else if (index == 11 || index == 12) {
            indexes[0]=11; indexes[1]=12; count=2; out[2]=0x16;
            if (v[11]<400 || v[11]>650 || v[12]<400 || v[12]>650 ||
                v[11]>=v[12]) return 0;
        } else {
            indexes[0]=36; indexes[1]=37; indexes[2]=40; count=3; out[2]=0x24;
            if (v[36]>100 || v[37]>100 || v[40]>100 || v[36]>=v[37]) return 0;
        }
        payload=count*2;
        for (size_t i=0; i<count; ++i) {
            if (!valid[indexes[i]]) return 0;
            out[3+i*2]=(uint8_t)(v[indexes[i]]>>8);
            out[4+i*2]=(uint8_t)v[indexes[i]];
        }
    }
    out[1]=(uint8_t)(payload+1);
    size_t len=payload+5;
    const uint16_t crc=direct_crc16(out,len-2);
    out[len-2]=(uint8_t)crc; out[len-1]=(uint8_t)(crc>>8);
    return len;
}

uint16_t direct_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    if (!data) return 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xa001U)
                             : (uint16_t)(crc >> 1);
    }
    return crc;
}

bool direct_dsp_validate(const uint8_t *frame, size_t len)
{
    if (!frame || len < 5 || len > DIRECT_DSP_MAX_FRAME || frame[0] != 0x01 ||
        frame[1] < 1 || len != (size_t)frame[1] + 4U)
        return false;
    return direct_crc16(frame, len - 2) ==
           (uint16_t)(((uint16_t)frame[len - 1] << 8) | frame[len - 2]);
}

size_t direct_dsp_make_request(uint8_t command, int32_t bms_current_ma,
                               uint16_t bms_voltage, uint16_t bms_soc,
                               uint8_t out[9])
{
    if (!out || command < 0x03 || command > 0x06) return 0;
    memset(out, 0, 9);
    out[0] = 0x01;
    out[1] = 5;
    out[2] = command;
    if (command == 0x05) {
        const uint32_t raw = (uint32_t)bms_current_ma;
        out[3] = (uint8_t)(raw >> 24);
        out[4] = (uint8_t)(raw >> 16);
        out[5] = (uint8_t)(raw >> 8);
        out[6] = (uint8_t)raw;
    } else if (command == 0x06) {
        out[3] = (uint8_t)(bms_voltage >> 8);
        out[4] = (uint8_t)bms_voltage;
        out[5] = (uint8_t)(bms_soc >> 8);
        out[6] = (uint8_t)bms_soc;
    }
    const uint16_t crc = direct_crc16(out, 7);
    out[7] = (uint8_t)crc;
    out[8] = (uint8_t)(crc >> 8);
    return 9;
}

bool direct_dsp_parse_cmd04(const uint8_t *frame, size_t len,
                            direct_dsp_measurement_t *out)
{
    /* Decode only the documented prefix; CRC covers any extension too. */
    if (!out || len < DIRECT_DSP_CMD04_LEN || !direct_dsp_validate(frame, len) ||
        frame[2] != 0x04)
        return false;
    direct_dsp_measurement_t m = {0};
    m.grid_v = be16(frame + 3);
    m.grid_i = (int16_t)be16(frame + 5);
    m.grid_hz_tenths = be16(frame + 7);
    m.grid_w = (int16_t)be16(frame + 9);
    m.grid_va = (int16_t)be16(frame + 11);
    m.grid_percent = be16(frame + 13);
    m.inverter_v = be16(frame + 15);
    m.inverter_i = (int16_t)be16(frame + 17);
    m.inverter_hz_tenths = be16(frame + 19);
    m.inverter_w = (int16_t)be16(frame + 21);
    m.inverter_va = (int16_t)be16(frame + 23);
    m.inverter_percent = be16(frame + 25);
    m.load_v = be16(frame + 27);
    m.load_i = (int16_t)be16(frame + 29);
    m.load_hz_tenths = be16(frame + 31);
    m.load_w = (int16_t)be16(frame + 33);
    m.load_va = (int16_t)be16(frame + 35);
    m.load_percent = be16(frame + 37);
    m.battery_v_tenths = be16(frame + 39);
    m.battery_i_tenths = (int16_t)be16(frame + 41);
    m.battery_w = (int16_t)be16(frame + 43);
    m.battery_soc = be16(frame + 45);
    m.bus_v_tenths = be16(frame + 47);
    m.bus_i = (int16_t)be16(frame + 49);
    m.pv1_v = be16(frame + 51);
    m.pv1_i = be16(frame + 53);
    m.pv2_v = be16(frame + 55);
    m.pv2_i = be16(frame + 57);
    m.pv1_w = be16(frame + 59);
    m.pv2_w = be16(frame + 61);
    m.pv_total_w = be16(frame + 63);
    m.inverter_temp_tenths = (int16_t)be16(frame + 65);
    *out = m;
    return true;
}

size_t direct_bms_make_query(uint8_t address, uint16_t start,
                             uint16_t words, uint8_t out[8])
{
    if (!out || address == 0 || words == 0 || words > 125) return 0;
    out[0] = address;
    out[1] = 0x03;
    out[2] = (uint8_t)(start >> 8);
    out[3] = (uint8_t)start;
    out[4] = (uint8_t)(words >> 8);
    out[5] = (uint8_t)words;
    const uint16_t crc = direct_crc16(out, 6);
    out[6] = (uint8_t)crc;
    out[7] = (uint8_t)(crc >> 8);
    return 8;
}

bool direct_bms_parse_modbus(const uint8_t *frame, size_t len,
                             uint8_t expected_address,
                             direct_bms_measurement_t *out)
{
    /* App(10) uses a 0x4A-byte payload. Its accesses at [77..80] cross
     * into the CRC/end of frame; do not carry that bug into this parser. */
    if (!frame || !out || len != 79 || frame[0] != expected_address ||
        frame[1] != 0x03 || frame[2] != 0x4a ||
        direct_crc16(frame, len - 2) !=
            (uint16_t)(((uint16_t)frame[len - 1] << 8) | frame[len - 2]))
        return false;
    direct_bms_measurement_t m = {0};
    m.address = expected_address;
    m.current_10ma = (int16_t)be16(frame + 3);
    m.voltage_10mv = be16(frame + 5);
    m.soc_percent = be16(frame + 7);
    m.soh_percent = be16(frame + 9);
    m.percent_divisor = 1;
    m.warning = be16(frame + 21);
    m.protection = be16(frame + 23);
    for (unsigned i = 0; i < 16; ++i)
        m.cell_mv[i] = be16(frame + 37 + i * 2);
    *out = m;
    return true;
}

size_t direct_bms_make_7c_query(uint8_t out[7])
{
    if (!out) return 0;
    out[0] = 0x7c;
    out[3] = 0x80;
    out[4] = 0x01;
    out[5] = 0;
    out[6] = 0;
    const uint16_t crc = direct_crc16(out + 3, 4);
    out[1] = (uint8_t)crc;
    out[2] = (uint8_t)(crc >> 8);
    return 7;
}

bool direct_bms_parse_7c(const uint8_t *frame, size_t len,
                         direct_bms_measurement_t *out)
{
    if (!frame || !out || len < 7 + 1 + 1 + 8 || len > 256 ||
        frame[0] != 0x7c || direct_crc16(frame + 3, len - 3) !=
            (uint16_t)(((uint16_t)frame[2] << 8) | frame[1]))
        return false;
    const uint8_t cells = frame[7];
    if (cells == 0 || cells > 16) return false;
    size_t pos = 8;
    if (pos + (size_t)cells * 2U + 1U > len) return false;
    direct_bms_measurement_t m = {0};
    m.address = 1;
    for (uint8_t i = 0; i < cells; ++i) {
        m.cell_mv[i] = (uint16_t)((frame[pos] | ((uint16_t)frame[pos + 1] << 8)) &
                                  0x1fffU);
        pos += 2;
    }
    const uint8_t temps = frame[pos++];
    if (temps > 16 || pos + (size_t)temps * 2U + 8U > len) return false;
    pos += (size_t)temps * 2U;
    m.current_10ma = (int32_t)(int16_t)(frame[pos] |
                      ((uint16_t)frame[pos + 1] << 8)) * 10;
    pos += 2;
    const uint32_t voltage = (uint32_t)(frame[pos] |
                             ((uint16_t)frame[pos + 1] << 8)) * 10U;
    if (voltage > UINT16_MAX) return false;
    m.voltage_10mv = (uint16_t)voltage;
    pos += 2;
    m.soc_percent = (uint16_t)(frame[pos] | ((uint16_t)frame[pos + 1] << 8));
    pos += 2;
    m.soh_percent = (uint16_t)(frame[pos] | ((uint16_t)frame[pos + 1] << 8));
    m.percent_divisor = 10;
    *out = m;
    return true;
}

static uint16_t bms_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void bms_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

bool direct_bms_encode_summary_7c(const uint8_t *frame, size_t len,
                                 uint8_t out[DIRECT_BMS_SUMMARY_SIZE])
{
    /* BBJ V1.6: header + cell count/words + temperature count/words +
     * packed 61-byte pack_info. Validate all boundaries before reading. */
    if (!frame || !out || len < 7U + 1U + 2U + 1U + 61U || len > 256U ||
        frame[0] != 0x7c || frame[3] != 0x80 || frame[4] != 0x01 ||
        bms_le16(frame + 5) != len - 7U ||
        direct_crc16(frame + 3, len - 3U) != bms_le16(frame + 1))
        return false;
    const unsigned cells = frame[7];
    if (cells == 0 || cells > 16) return false;
    const size_t temp_count_pos = 8U + cells * 2U;
    if (temp_count_pos >= len) return false;
    const unsigned temps = frame[temp_count_pos];
    if (temps > 16) return false;
    const size_t pack_pos = temp_count_pos + 1U + temps * 2U;
    if (pack_pos + 61U != len) return false;
    const uint8_t *pack = frame + pack_pos;
    const uint32_t voltage = (uint32_t)bms_le16(pack + 2) * 10U;
    if (voltage > UINT16_MAX) return false;

    uint8_t bytes[DIRECT_BMS_SUMMARY_SIZE] = {0};
    bytes[0] = (uint8_t)cells; /* Same cell-count meaning as ARM batteryCnt. */
    bms_put16(bytes + 1, (uint16_t)voltage);
    const uint32_t current = (uint32_t)((int32_t)(int16_t)bms_le16(pack) * 10);
    for (unsigned i = 0; i < 4; ++i) bytes[3 + i] = (uint8_t)(current >> (8U * i));
    memcpy(bytes + 7, pack + 4, 8); /* SOC/SOH and capacities: original 0.1 units. */
    bms_put16(bytes + 15, bms_le16(pack + 12)); /* u16 BBJ -> u32 L10. */

    uint16_t balance = 0, max_cell = 0, min_cell = UINT16_MAX;
    for (unsigned i = 0; i < cells; ++i) {
        const uint16_t raw = bms_le16(frame + 8U + i * 2U);
        const uint16_t mv = raw & 0x1fffU;
        bms_put16(bytes + 27U + i * 2U, mv);
        if (raw & 0x8000U) balance |= (uint16_t)(1U << i);
        if (mv > max_cell) max_cell = mv;
        if (mv < min_cell) min_cell = mv;
    }
    bms_put16(bytes + 25, balance);
    bms_put16(bytes + 69, max_cell);
    bms_put16(bytes + 71, min_cell);

    unsigned cell_temps = 0;
    int16_t min_temp = INT16_MAX, max_temp = INT16_MIN;
    for (unsigned i = 0; i < temps; ++i) {
        const uint8_t *raw = frame + temp_count_pos + 1U + i * 2U;
        const unsigned type = (raw[1] >> 5) & 3U;
        const int16_t celsius = (int16_t)raw[0] - 40;
        if (type == 0) {
            if (cell_temps < 4U) bms_put16(bytes + 59U + cell_temps * 2U, (uint16_t)celsius);
            ++cell_temps;
            if (celsius < min_temp) min_temp = celsius;
            if (celsius > max_temp) max_temp = celsius;
        } else {
            /* Types: environment=1, PCB=2, MOS=3. Never assume array order. */
            const unsigned offset = type == 1 ? 81U : type == 2 ? 79U : 77U;
            bms_put16(bytes + offset, (uint16_t)celsius);
        }
    }
    if (cell_temps) {
        bms_put16(bytes + 73, (uint16_t)max_temp);
        bms_put16(bytes + 75, (uint16_t)min_temp);
    }
    memcpy(bytes + 67, pack + 28, 2); /* cycles */
    bytes[83] = pack[14];
    bytes[84] = pack[15];
    memcpy(bytes + 85, pack + 30, 8); /* total charge/discharge capacity */
    memcpy(bytes + 93, pack + 56, 4); /* requested current/voltage */
    bytes[97] = pack[60];
    /* BBJ's 32-bit fault/64-bit alarm status is not the L10 bit layout.
     * Bytes 19..24 and the absent charging_voltage stay reserved zero. */
    memcpy(out, bytes, sizeof(bytes));
    return true;
}

static uint8_t boot_checksum(const uint8_t *bytes, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum = (uint8_t)(sum + bytes[i]);
    return (uint8_t)(0xffU - sum);
}

size_t direct_bms_boot_make_frame(const uint8_t *payload, size_t payload_len,
                                  uint8_t *out, size_t out_capacity)
{
    if (!payload || !out || payload_len == 0 || payload_len > 4099U ||
        out_capacity < payload_len + 7U)
        return 0;
    const size_t frame_len = payload_len + 7U;
    out[0] = 0x5a;
    out[1] = 0xff;
    out[2] = 0x80;
    out[3] = (uint8_t)(payload_len >> 8);
    out[4] = (uint8_t)payload_len;
    memcpy(out + 5, payload, payload_len);
    out[5 + payload_len] = boot_checksum(out + 1, payload_len + 4U);
    out[6 + payload_len] = 0xa5;
    return frame_len;
}

bool direct_bms_boot_parse_reply(const uint8_t *frame, size_t len,
                                 uint8_t expected_command,
                                 direct_bms_boot_reply_t *out)
{
    if (!frame || !out || len != 15 || frame[0] != 0x5a || frame[14] != 0xa5 ||
        frame[1] != 0x80 || frame[2] != 0xff || frame[3] != 0 ||
        frame[4] != 8 || frame[5] != expected_command ||
        boot_checksum(frame + 1, 12) != frame[13])
        return false;
    direct_bms_boot_reply_t reply = {
        .command = frame[5],
        .device_type = frame[6],
        .firmware_type = frame[7],
        .error = frame[8],
        .boot_type = frame[9],
        .hardware_type_high = frame[10],
        .hardware_type_low = frame[11],
        .block_size_low = frame[12],
    };
    *out = reply;
    return true;
}
