// @action read_and_send
// @node sampler.Sampling (entry)
// @stub sends a counter instead of reading the sensors

#include <stdint.h>

#include "gen/nodes.h"

static uint32_t count;

/* The payload is opaque to stategate: here a 4-byte counter, on the device an
 * encoded protobuf message. */
void sampler_read_and_send_run(sampler_safe_hal_t *hal) {
    count++;
    sampler_send_tick(hal, &count, sizeof count);
}
