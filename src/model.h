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

/* Hardware a node touches. stategate knows no parts: only the comm type,
 * the bus, and an address or pin, enough to catch two nodes claiming the
 * same pin or the same address on one bus. */
typedef struct {
    char *comm; /* i2c | spi | uart | gpio */
    char *bus;  /* a bus id from `buses` (not for gpio) */
    long addr;  /* i2c: 7-bit address, -1 when absent */
    char *cs;   /* spi: chip-select pin */
    char *pin;  /* gpio */
    int line;
} Resource;

/* A bus and the pins it holds. A bus behind a mux names its parent with
 * `via` and holds no pins of its own. */
typedef struct {
    char *id, *comm, *via;
    StrList roles, pins; /* parallel: "sda" -> "GPIO3" */
    int line;
} Bus;

typedef struct {
    char *id;
    int stub, locked;
    char *note;
    Resource *uses;
    int n_uses;
    int line;
} Action;

typedef struct {
    char *id;
    int stub, locked;
    char *fidelity;
    char *note;
    StrList emits;
    Resource *uses;
    int n_uses;
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
    StrList sends; /* events this machine sends to others (system.yaml channels) */
    StrList params; /* per-instance integers the nodes read (values in system.yaml) */
    HalCall *hal;
    int n_hal;
    Bus *buses;
    int n_buses;
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

/* Languages stategate generates code for; a project uses exactly one. */
extern const char *const SG_LANGUAGES[];

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
