// @action stim_off
// @node posture.Off (entry)
// @node posture.Nudging (exit)

use crate::gen::hal::{Hal, SafeHal};

/// Stops all stimulation. Locked: runs on entry to Off and on every exit from Nudging.
pub fn run<H: Hal>(hal: &mut SafeHal<H>) {
    hal.stim_set_intensity(0);
}
