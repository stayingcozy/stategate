// @action report_full
// @node logger.Full (entry)
// @stub send LOG_FULL to the device

#include "gen/nodes.h"

void logger_report_full_run(logger_safe_hal_t *hal) {
    logger_hal_log(hal, "report_full (stub)");
}
