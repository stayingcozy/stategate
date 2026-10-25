// @service imu_posture
// @node posture.Monitoring (invoke)
// @node posture.Slouching (invoke)
// @node posture.Nudging (invoke)
// @stub Step 0: report a slouch at t = 10 s

use crate::gen::hal::{Hal, SafeHal};
use crate::gen::machine::Event;

#[derive(Default)]
pub struct ImuPosture {}

impl ImuPosture {
    /// Called on every tick while an invoking state is active.
    pub fn poll<H: Hal>(&mut self, hal: &mut SafeHal<H>) -> Option<Event> {
        let _ = hal;
        None
    }
}
