// @action stim_off
// @node posture.Off (entry)
// @node posture.Nudging (exit)

#include "gen/nodes.h"

/* Stops all stimulation. Locked: runs on entry to Off and on every exit from Nudging. */
void posture_stim_off_run(posture_safe_hal_t *hal) {
    posture_hal_stim_set_intensity(hal, 0);
}
