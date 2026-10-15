#ifndef DIRECT_DISPLAY_EVENTS_H
#define DIRECT_DISPLAY_EVENTS_H
#include "direct_ui.h"
/* Hash only the visible view and scene inputs. Reception timestamps and
 * wall-clock seconds do not turn identical data into a repaint event. */
uint32_t direct_display_signature(const direct_ui_state_t *ui,
                                  const direct_ui_data_t *data,
                                  const direct_ui_view_t *view);
#endif
