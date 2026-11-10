// @action start_nudge
// @node posture.Nudging (entry)
// @stub Step 0: log NUDGE and request intensity 5 (the safe HAL clamps it)

#include "gen/nodes.h"

void posture_start_nudge_run(posture_safe_hal_t *hal) {
    posture_hal_log(hal, "NUDGE");
    posture_hal_stim_set_intensity(hal, 5);
}
