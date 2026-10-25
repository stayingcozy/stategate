#ifndef SG_TICKET_H
#define SG_TICKET_H

#include "util.h"

#define TICKETS_DIR ".fsm/tickets"
#define MAX_FAILURES 3

/* A scoped unit of work for one agent. Status: open | active | done |
 * locked | split. At most one ticket is active or locked at a time. */
typedef struct {
    char *file;
    char *id, *title, *step, *status, *base;
    int failures;
    StrList nodes; /* node IDs the work changes; the changelog must name them */
    StrList files; /* globs the work may touch */
} Ticket;

typedef struct {
    Ticket *v;
    int n;
} Tickets;

int tickets_load(Tickets *ts, StrList *errors);
int ticket_save(const Ticket *t);
Ticket *ticket_find(Tickets *ts, const char *id);
/* The ticket in force: status active or locked. NULL if none. */
Ticket *ticket_current(Tickets *ts);

int cmd_ticket(int argc, char **argv);

#endif
