// @service calibrate
// @node sensor.Calibrating (invoke)
// @stub reports CALIBRATED at once, after logging its mux channel

#include <stdio.h>

#include "gen/nodes.h"

bool sensor_calibrate_poll(sensor_safe_hal_t *hal, sensor_event_t *event) {
    char line[48];
    snprintf(line, sizeof line, "calibrating on mux channel %d", (int)sensor_hal_param_mux_ch(hal));
    sensor_hal_log(hal, line);
    *event = SENSOR_EV_CALIBRATED;
    return true;
}
