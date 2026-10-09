#ifndef OTA_PROGRESS_H
#define OTA_PROGRESS_H

#include "ota_service.h"
#include <string.h>

static inline uint8_t ota_progress_clamp(int percent)
{
    return (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent);
}

static inline uint8_t ota_collector_overall_progress(int percent)
{
    return (uint8_t)(70 + (ota_progress_clamp(percent) * 29) / 100);
}

static inline uint8_t ota_receiving_overall_progress(OtaTarget target, int percent)
{
    const int weight = target == OTA_TARGET_ESP ? 95 : 70;
    return (uint8_t)((ota_progress_clamp(percent) * weight) / 100);
}

static inline void ota_snapshot_progress(OtaSnapshot* snapshot, const char* stage,
                                         int stage_progress, int overall_progress)
{
    const size_t len = strlen(stage);
    const size_t count = len < sizeof(snapshot->stage) - 1 ? len : sizeof(snapshot->stage) - 1;
    memcpy(snapshot->stage, stage, count);
    snapshot->stage[count] = '\0';
    snapshot->stage_progress = ota_progress_clamp(stage_progress);
    snapshot->overall_progress = ota_progress_clamp(overall_progress);
    snapshot->progress = snapshot->overall_progress;
}

#endif
