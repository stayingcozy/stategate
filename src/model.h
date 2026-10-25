#ifndef SG_MODEL_H
#define SG_MODEL_H

#include "util.h"

typedef struct {
    char *event;
    char *target;
} Transition;

typedef struct {
    char *id;   /* stable, e.g. posture.Nudging */
    char *name; /* display name, also the generated enum variant */
    int initial;
    StrList entry, exit, ignore, invoke;
    Transition *on;
    int n_on;
    int has_after;
    long after_ms;
    char *after_event;
    double x, y;
    int line;
} State;

typedef struct {
    char *id;
    int stub, locked;
    char *note;
    int line;
} Action;

typedef struct {
    char *id;
    int stub, locked;
    char *fidelity;
    char *note;
    StrList emits;
    int line;
} Service;

typedef enum {
    INV_UNKNOWN,
    INV_EVERY_STATE_HANDLES,
    INV_MAX_TIME_IN_STATE,
    INV_HAL_CLAMP,
} InvKind;

typedef struct {
    char *id;
    int locked;
    InvKind kind;
    char *kind_name;
    char *event, *target;             /* every_state_handles */
    char *state, *exit_event;         /* max_time_in_state */
    long limit_ms;
    char *call;                       /* hal_clamp */
    long max;
    int line;
} Invariant;

typedef struct {
    char *id, *goal, *status;
} Step;

typedef struct {
    char *call;
    char *arg; /* u8 | u16 | u32 | i32 | bool */
} HalCall;

typedef struct {
    char *path;
    char *hash; /* sha256 of the file bytes */
    char *id;
    long version;
    char *language;
    StrList events;
    HalCall *hal;
    int n_hal;
    State *states;
    int n_states;
    Action *actions;
    int n_actions;
    Service *services;
    int n_services;
    Invariant *invariants;
    int n_invariants;
    Step *steps;
    int n_steps;
} SgModel;

/* Parses the model and records schema errors (gate check 1). Returns 0
 * when the file parsed with no schema errors. */
int model_load(const char *path, SgModel *m, StrList *errors);

State *model_state(const SgModel *m, const char *id);
Action *model_action(const SgModel *m, const char *id);
Service *model_service(const SgModel *m, const char *id);
HalCall *model_hal(const SgModel *m, const char *call);
int model_event_index(const SgModel *m, const char *event);
/* Target of (state, event), or NULL if not handled. */
const char *state_target(const State *s, const char *event);

/* Finds the single *.yaml model in dir. Returns a malloc'd path or NULL. */
char *model_find(const char *dir, StrList *errors);

#endif
