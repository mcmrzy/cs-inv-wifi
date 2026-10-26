#ifndef DIRECT_DSP_PARAMS_H
#define DIRECT_DSP_PARAMS_H
#include "direct_protocol.h"
#include "direct_battery_config.h"
#define DIRECT_DSP_PARAMS_BUNDLE_SIZE 64U
#define DIRECT_DSP_PARAMS_RECORD_SIZE 92U
#define DIRECT_DSP_PROFILES_RECORD_SIZE (DIRECT_DSP_PARAMS_RECORD_SIZE+4U+DIRECT_BATTERY_PROFILE_COUNT*28U+2U)
typedef struct {
    uint8_t frames[DIRECT_DSP_PARAMS_BUNDLE_SIZE]; bool buzzer; uint32_t revision;
    uint8_t battery_profiles_valid, battery_material;
    direct_battery_config_t battery_profiles[DIRECT_BATTERY_PROFILE_COUNT];
} direct_dsp_params_t;
bool direct_dsp_params_battery_get(const direct_dsp_params_t *params,direct_battery_config_t *config);
direct_battery_result_t direct_dsp_params_battery_set(direct_dsp_params_t *params,const direct_battery_config_t *config);
size_t direct_dsp_params_charge_reply(const direct_dsp_params_t *params,uint8_t out[11]);
bool direct_dsp_params_profiles_pack(const direct_dsp_params_t *params,const char *sn,uint8_t out[DIRECT_DSP_PROFILES_RECORD_SIZE]);
bool direct_dsp_params_profiles_unpack(const uint8_t *record,size_t length,const char *sn,direct_dsp_params_t *params);
/* Original ARM factory initialization, explicitly authorized for new records.
 * These are source defaults, not settings recovered from a physical ARM. */
bool direct_dsp_params_arm_defaults(direct_dsp_params_t *params);
/* Desired user settings, in native DSP units. Unsupported/inactive fields
 * stay unavailable; setters preserve every other field and all reserved bits. */
bool direct_dsp_params_get(const direct_dsp_params_t *params,uint16_t index,uint16_t *value);
bool direct_dsp_params_set(direct_dsp_params_t *params,uint16_t index,uint16_t value);
/* Exact original ARM frames 00(25B),01(23B),02(16B). */
bool direct_dsp_params_decode(const uint8_t *bundle,size_t length,direct_dsp_params_t *params);
size_t direct_dsp_params_reply(const direct_dsp_params_t *params,uint8_t command,uint8_t out[25]);
bool direct_dsp_params_pack(const direct_dsp_params_t *params,const char *sn,uint8_t out[DIRECT_DSP_PARAMS_RECORD_SIZE]);
bool direct_dsp_params_unpack(const uint8_t *record,size_t length,const char *sn,direct_dsp_params_t *params);
bool direct_dsp_params_write_matches(const uint8_t *write,size_t write_length,const uint8_t *readback,size_t readback_length);
/* Only a fully matched explicit write changes persistence. Runtime 25 data
 * never replaces the user's current limits or unrelated settings. */
bool direct_dsp_params_apply_write(direct_dsp_params_t *params,uint16_t requested_index,const uint8_t *write,size_t write_length,const uint8_t *readback,size_t readback_length);
/* Supplied DSP TX-buffer collision signature. This is a resync hint, never
 * a valid incoming frame, parameter source, or measurement. */
bool direct_dsp_params_collision_hint(const uint8_t *bytes,size_t length);
#endif
