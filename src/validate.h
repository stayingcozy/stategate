#ifndef SG_VALIDATE_H
#define SG_VALIDATE_H

#include "model.h"

/* Gate check 2: the model is a valid, total statechart. */
void check_statechart(const Model *m, StrList *errors);
/* Gate check 3: locked invariants hold on the model itself. */
void check_model_invariants(const Model *m, StrList *errors);

/* Loads and runs checks 1-3. Returns 0 when all pass. Prints nothing. */
int validate_all(const char *path, Model *m, StrList *e1, StrList *e2, StrList *e3);

#endif
