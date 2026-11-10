// @service storage
// @node logger.Logging (invoke)
// @stub watch free space; emit DISK_FULL below 1 MB

#include "gen/nodes.h"

bool logger_storage_poll(logger_safe_hal_t *hal, logger_event_t *event) {
    (void)hal;
    (void)event;
    return false;
}
