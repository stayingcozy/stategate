// @service imu_posture
// @node posture.Monitoring (invoke)
// @node posture.Slouching (invoke)
// @node posture.Nudging (invoke)
// @stub Step 0: report a slouch at t = 10 s

use crate::gen::hal::{Hal, SafeHal};
use crate::gen::machine::Event;

const SLOUCH_AT_MS: u64 = 10_000;

#[derive(Default)]
pub struct ImuPosture {
    reported: bool,
}

impl ImuPosture {
    /// Called on every tick while an invoking state is active.
    pub fn poll<H: Hal>(&mut self, hal: &mut SafeHal<H>) -> Option<Event> {
        if !self.reported && hal.now_ms() >= SLOUCH_AT_MS {
            self.reported = true;
            return Some(Event::Slouch);
        }
        None
    }
}
