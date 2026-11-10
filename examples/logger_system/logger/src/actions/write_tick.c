// @action write_tick
// @node logger.Logging (entry)
// @stub logs the tick's counter instead of writing a file

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gen/nodes.h"

void logger_write_tick_run(logger_safe_hal_t *hal) {
    size_t len;
    const void *data = logger_hal_payload(hal, &len);
    uint32_t n = 0;
    if (data && len == sizeof n) memcpy(&n, data, sizeof n);
    if (n % 100 == 0) {
        char line[48];
        snprintf(line, sizeof line, "wrote tick %u", (unsigned)n);
        logger_hal_log(hal, line);
    }
}
