#ifndef SG_RESOURCES_H
#define SG_RESOURCES_H

#include "model.h"

/* One node's claim on a piece of hardware. */
typedef struct {
    const char *owner; /* e.g. "logger.storage" */
    const Resource *r;
} Claim;

/* Checks the buses and the claims on them: each bus is complete, each claim
 * names a bus of its comm type with an address or pin, no pin is used twice
 * (bus lines, chip selects and GPIOs together) and no two nodes share an
 * address on one bus. Appends one message per problem. */
void resources_check(const Bus *buses, int n_buses, const Claim *claims, int n_claims, StrList *errors);

/* Every claim in one model, owned by "<machine>.<node>". Returns the count;
 * *out is malloc'd. */
int model_claims(const SgModel *m, Claim **out);

#endif
