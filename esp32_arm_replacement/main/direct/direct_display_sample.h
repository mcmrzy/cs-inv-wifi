#ifndef DIRECT_DISPLAY_SAMPLE_H
#define DIRECT_DISPLAY_SAMPLE_H

#include <stdbool.h>
#include <stdint.h>
#include "arm_param.h"

/* Maximum age of an incoming sample. Cached LCD values have no expiry;
 * callers must label stale values. Cloud telemetry retains its freshness checks. */
#define DIRECT_DISPLAY_SAMPLE_HOLD_MS 5000U

typedef struct {
    RunParamDef measurement;
    uint32_t received_ms, blocked_ms;
    bool valid, waiting_after_pause;
} direct_display_sample_t;

/* Returns availability of the last real sample, not communication freshness.
 * A false getter result never refreshes the stored reception timestamp.
 * Pausing clears the cache and requires a sample received after that pause. */
bool direct_display_sample_update(direct_display_sample_t *sample,
                                 uint32_t now_ms, bool paused,
                                 bool received, const RunParamDef *measurement,
                                 uint32_t received_ms, RunParamDef *display,
                                 uint32_t *age_ms);
/* Time passing alone is never a repaint trigger. Kept for callers/tests;
 * panel task separately compares visible-content signatures. */
bool direct_display_should_render(uint32_t now_ms, uint32_t last_render_ms,
                                  bool dirty);

#endif
