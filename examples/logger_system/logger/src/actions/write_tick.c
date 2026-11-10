// @action write_tick
// @node logger.Logging (entry)
// @stub append the tick to the open file

#include "gen/nodes.h"

void logger_write_tick_run(logger_safe_hal_t *hal) {
    logger_hal_log(hal, "write_tick (stub)");
}
