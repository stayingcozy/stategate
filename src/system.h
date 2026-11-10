#ifndef SG_SYSTEM_H
#define SG_SYSTEM_H

#include "model.h"

#define SYSTEM_PATH "system.yaml"

/* A system is several machines, each a stategate project in its own folder
 * (its model, generated code, tickets and gate), wired together by channels
 * declared in system.yaml at the root. */
typedef struct {
    char *id;
    char *dir; /* relative to the system root */
    SgModel model;
    int loaded; /* model loaded and passed checks 1-3 */
    int instances; /* copies of this machine, each with its own state (default 1) */
    StrList param_names;
    long *param_values; /* [instance][param], param_names.n per instance */
    int line;
} SysMachine;

typedef struct {
    char *from, *to, *event;
    char *kind; /* queue | sync */
    long depth; /* queue only */
    char *drop; /* queue only: never | oldest | newest */
    long payload; /* most payload bytes one event carries; 0 = none */
    int line;
} Channel;

typedef struct {
    char *path;
    char *id;
    char *language;
    SysMachine *machines;
    int n_machines;
    Channel *channels;
    int n_channels;
    StrList external; /* events that arrive from outside the system: ISR, phone, host */
} SgSystem;

/* Parses system.yaml and loads each machine's model from its folder.
 * Schema problems and machines that fail checks 1-3 go to errors (prefixed
 * with the machine id). Returns 0 when everything loaded cleanly. */
int system_load(const char *path, SgSystem *s, StrList *errors);

/* The wiring checks across machines. Appends one message per problem. */
void system_check(const SgSystem *s, StrList *errors);

SysMachine *system_machine(const SgSystem *s, const char *id);

/* `stategate validate|gen|gate|init|export mermaid` at a system root. */
int cmd_system(const char *cmd, int argc, char **argv, const char *argv0);

#endif
