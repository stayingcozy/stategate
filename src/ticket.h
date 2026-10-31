#ifndef SG_TICKET_H
#define SG_TICKET_H

#include "util.h"

#define TICKETS_DIR ".fsm/tickets"
#define MAX_FAILURES 3

/* A scoped unit of work for one agent. Status: draft | open | approved |
 * active | done | locked | split. `ticket gen` writes drafts; filling in the
 * title and goal makes one open. At most one ticket is active or locked at a time;
 * approved tickets wait in id order and start one by one. */
typedef struct {
    char *file;
    char *id, *title, *goal, *step, *status, *base;
    char *approval; /* set by `ticket start` when the architect approves */
    int failures;
    StrList nodes; /* node IDs the work changes; the changelog must name them */
    StrList files; /* globs the work may touch */
    StrList errors; /* what the last red gate run said; empty after a green one */
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

/* Hash of everything the architect approves: id, title, goal, step, base,
 * nodes and files. */
void ticket_approval_hash(const Ticket *t, char out[65]);
/* Typed confirmation at an interactive terminal; 0 when refused. */
int confirm_by_typing(const char *phrase);

/* model_path may be NULL when no model was found; subcommands that need
 * the model then refuse. */
int cmd_ticket(int argc, char **argv, const char *model_path);

#endif
