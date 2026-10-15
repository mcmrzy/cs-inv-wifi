#include "direct_display_sample.h"
#include <string.h>

bool direct_display_sample_update(direct_display_sample_t *sample,
                                 uint32_t now_ms, bool paused,
                                 bool received, const RunParamDef *measurement,
                                 uint32_t received_ms, RunParamDef *display,
                                 uint32_t *age_ms)
{
    if (!sample || !display || !age_ms) return false;
    if (paused) {
        memset(&sample->measurement, 0, sizeof(sample->measurement));
        sample->valid = false;
        sample->waiting_after_pause = true;
        sample->blocked_ms = now_ms;
    } else if (received && measurement &&
               (uint32_t)(now_ms - received_ms) < DIRECT_DISPLAY_SAMPLE_HOLD_MS &&
               (!sample->waiting_after_pause ||
                (int32_t)(received_ms - sample->blocked_ms) > 0)) {
        sample->measurement = *measurement;
        sample->received_ms = received_ms;
        sample->valid = true;
        sample->waiting_after_pause = false;
    }
    *age_ms = sample->valid ? (uint32_t)(now_ms - sample->received_ms) : UINT32_MAX;
    if (sample->valid) {
        *display = sample->measurement;
        return true;
    }
    memset(display, 0, sizeof(*display));
    return false;
}

bool direct_display_should_render(uint32_t now_ms, uint32_t last_render_ms,
                                  bool dirty)
{
    (void)now_ms;
    return dirty || !last_render_ms;
}
