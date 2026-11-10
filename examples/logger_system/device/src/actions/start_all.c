// @action start_all
// @node device.Recording (entry)

#include "gen/nodes.h"

/* START reaches the sampler and the logger through system.yaml's sync channels. */
void device_start_all_run(device_safe_hal_t *hal) {
    device_send_start(hal, NULL, 0);
}
