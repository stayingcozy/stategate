#ifndef SG_TRACE_H
#define SG_TRACE_H

#include <stdint.h>

#include "model.h"

/* A transition log written by generated code (see src/gen/trace.h in a C
 * project): "SGT1", a machine count, each machine's id and model hash, then
 * 11-byte little-endian records. */
typedef struct {
    uint32_t t_ms;
    uint16_t seq;
    uint8_t machine, instance, from, event, to;
} TraceRec;

typedef struct {
    int n_machines;
    char ids[32][64];
    char hashes[32][65]; /* hex */
    TraceRec *recs;
    int n;
} SgTrace;

/* Returns 0 on success; otherwise writes a reason to err. */
int trace_load(const char *path, SgTrace *log, char *err, size_t errn);
/* The header entry whose model hash is the model's, or -1. */
int trace_machine_for(const SgTrace *log, const SgModel *m);
/* "<from> --EVENT--> <to>" with the model's names; "?" for numbers it lacks. */
void trace_describe(const SgModel *m, const TraceRec *r, char *out, size_t n);

/* `stategate trace show <file>`: prints each record by name. Refuses a log
 * whose machines' model hashes match none of models. */
int trace_show(const char *path, const SgModel *const *models, int n_models);

#endif
