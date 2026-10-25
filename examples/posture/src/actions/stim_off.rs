// @action stim_off
// @node posture.Off (entry)
// @node posture.Nudging (exit)
// @stub placeholder written by stategate; replace with real logic

use crate::gen::hal::{Hal, SafeHal};

pub fn run<H: Hal>(hal: &mut SafeHal<H>) {
    hal.log("stim_off (stub)");
}
