#ifndef SG_GATE_STATUS_H
#define SG_GATE_STATUS_H

#include "util.h"

#define GATE_MAX_CHECKS 16

typedef struct {
    char *name, *status; /* pass | fail | skip */
    StrList messages;
} GateCheck;

/* What the last `stategate gate` run wrote to .fsm/gate.json. The viewer
 * shows this and nothing else as status. */
typedef struct {
    int present;
    int ok, release;
    char *time, *ticket, *ticket_status, *step;
    int failures, max_failures, stub_count;
    StrList stubs;
    GateCheck checks[GATE_MAX_CHECKS];
    int n_checks;
    StrList errors;
} GateStatus;

void gate_status_load(GateStatus *g, const char *path);
void gate_status_free(GateStatus *g);

#endif
