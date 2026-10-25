// @action start_nudge
// @node posture.Nudging (entry)
// @stub Step 0: log NUDGE and request intensity 5 (SafeHal clamps it)

use crate::gen::hal::{Hal, SafeHal};

pub fn run<H: Hal>(hal: &mut SafeHal<H>) {
    hal.log("NUDGE");
    hal.stim_set_intensity(5);
}
