#pragma once
#include "factory_record.h"
#include "cJSON.h"

/* No credentials belong in either USB inspection or MQTT identity reports. */
bool factory_record_json(cJSON *object, const factory_record_t *record);
cJSON *factory_info_json(const char *runtime_sn, const char *firmware,
                         const char *mac, factory_result_t status,
                         const factory_record_t *record,
                         uint32_t runtime_generation, bool pending_override);
