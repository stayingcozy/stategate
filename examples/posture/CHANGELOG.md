# Changelog

Each entry is headed by its ticket ID and names the node IDs it changed.

## T001
Step 0 node logic.
- posture.Off, posture.Nudging: `stim_off` implemented for real (intensity 0). Locked.
- posture.Nudging: `start_nudge` stub logs NUDGE and requests intensity 5; SafeHal clamps it to 3.
- posture.Monitoring: `imu_posture` stub reports one slouch at t = 10 s.
