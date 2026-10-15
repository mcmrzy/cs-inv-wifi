#ifndef DIRECT_PROTOCOL_H
#define DIRECT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ARM App(10): [0]=0x01, [1]=command+payload length, [2]=command,
 * CRC16/Modbus low byte first. Length on the wire is byte[1]+4. */
#define DIRECT_DSP_MAX_FRAME 128U
#define DIRECT_DSP_CMD04_LEN 69U
#define DIRECT_DSP_CONFIG_COUNT 84U

typedef struct {
    uint32_t fault;
    uint16_t inverter_status[3];
    uint16_t warning[3];
    uint8_t mode;
} direct_dsp_status_t;

bool direct_dsp_parse_cmd03(const uint8_t *frame, size_t len,
                          direct_dsp_status_t *out);
bool direct_dsp_parse_cmd25(const uint8_t *frame, size_t len,
                          uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                          bool valid[DIRECT_DSP_CONFIG_COUNT]);
/* Only commands active in App(10) ProcessDspCom are accepted. Values retain
 * the original Collector units; paired writes require both known values. */
bool direct_dsp_config_writable(uint16_t index);
size_t direct_dsp_make_config_write(uint16_t index, uint16_t value,
                          const uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                          const bool valid[DIRECT_DSP_CONFIG_COUNT],
                          uint8_t out[16]);
size_t direct_dsp_make_charge_write(uint16_t bulk, uint16_t floating,
                                    uint16_t cutoff, uint8_t out[11]);

typedef struct {
    uint16_t grid_v;
    int16_t grid_i;
    uint16_t grid_hz_tenths;
    int16_t grid_w;
    int16_t grid_va;
    uint16_t grid_percent;
    uint16_t inverter_v;
    int16_t inverter_i;
    uint16_t inverter_hz_tenths;
    int16_t inverter_w;
    int16_t inverter_va;
    uint16_t inverter_percent;
    uint16_t load_v;
    int16_t load_i;
    uint16_t load_hz_tenths;
    int16_t load_w;
    int16_t load_va;
    uint16_t load_percent;
    uint16_t battery_v_tenths;
    int16_t battery_i_tenths;
    int16_t battery_w;
    uint16_t battery_soc;
    uint16_t bus_v_tenths;
    int16_t bus_i;
    uint16_t pv1_v;
    uint16_t pv1_i;
    uint16_t pv2_v;
    uint16_t pv2_i;
    uint16_t pv1_w;
    uint16_t pv2_w;
    uint16_t pv_total_w;
    int16_t inverter_temp_tenths;
} direct_dsp_measurement_t;

typedef struct {
    uint8_t address;
    int32_t current_10ma;
    uint16_t voltage_10mv;
    uint16_t soc_percent;
    uint16_t soh_percent;
    uint16_t warning;
    uint16_t protection;
    uint16_t cell_mv[16];
} direct_bms_measurement_t;

uint16_t direct_crc16(const uint8_t *data, size_t len);
bool direct_dsp_validate(const uint8_t *frame, size_t len);
size_t direct_dsp_make_request(uint8_t command, int32_t bms_current_ma,
                               uint16_t bms_voltage, uint16_t bms_soc,
                               uint8_t out[9]);
bool direct_dsp_parse_cmd04(const uint8_t *frame, size_t len,
                            direct_dsp_measurement_t *out);
size_t direct_bms_make_query(uint8_t address, uint16_t start,
                             uint16_t words, uint8_t out[8]);
bool direct_bms_parse_modbus(const uint8_t *frame, size_t len,
                             uint8_t expected_address,
                             direct_bms_measurement_t *out);
size_t direct_bms_make_7c_query(uint8_t out[7]);
bool direct_bms_parse_7c(const uint8_t *frame, size_t len,
                         direct_bms_measurement_t *out);

/* BMS Boot(9)/bmsBoot(7) wire codec only. No live OTA transport is enabled. */
typedef struct {
    uint8_t command;
    uint8_t device_type;
    uint8_t firmware_type;
    uint8_t error;
    uint8_t boot_type;
    uint8_t hardware_type_high;
    uint8_t hardware_type_low;
    uint8_t block_size_low;
} direct_bms_boot_reply_t;

size_t direct_bms_boot_make_frame(const uint8_t *payload, size_t payload_len,
                                  uint8_t *out, size_t out_capacity);
bool direct_bms_boot_parse_reply(const uint8_t *frame, size_t len,
                                 uint8_t expected_command,
                                 direct_bms_boot_reply_t *out);

#endif
