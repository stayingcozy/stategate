#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trace_load(const char *path, SgTrace *log, char *err, size_t errn) {
    memset(log, 0, sizeof *log);
    size_t len;
    char *raw = read_file(path, &len);
    const uint8_t *b = (const uint8_t *)raw;
    if (!raw) { snprintf(err, errn, "cannot read %s", path); return -1; }
    if (len < 5 || memcmp(b, "SGT1", 4)) { snprintf(err, errn, "%s is not a stategate trace (no SGT1 header)", path); free(raw); return -1; }
    size_t at = 5;
    log->n_machines = b[4];
    if (log->n_machines > 32) { snprintf(err, errn, "%s: too many machines in the header", path); free(raw); return -1; }
    for (int i = 0; i < log->n_machines; i++) {
        if (at >= len) goto short_file;
        size_t n = b[at++];
        if (at + n + 32 > len || n >= sizeof log->ids[i]) goto short_file;
        memcpy(log->ids[i], b + at, n);
        log->ids[i][n] = 0;
        at += n;
        for (int k = 0; k < 32; k++) snprintf(log->hashes[i] + 2 * k, 3, "%02x", b[at + k]);
        at += 32;
    }
    log->n = (int)((len - at) / 11);
    log->recs = xcalloc((size_t)(log->n ? log->n : 1), sizeof(TraceRec));
    for (int i = 0; i < log->n; i++, at += 11) {
        TraceRec *r = &log->recs[i];
        r->t_ms = (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
        r->seq = (uint16_t)(b[at + 4] | b[at + 5] << 8);
        r->machine = b[at + 6];
        r->instance = b[at + 7];
        r->from = b[at + 8];
        r->event = b[at + 9];
        r->to = b[at + 10];
    }
    free(raw);
    return 0;
short_file:
    snprintf(err, errn, "%s: the header is cut short", path);
    free(raw);
    return -1;
}

int trace_machine_for(const SgTrace *log, const SgModel *m) {
    for (int i = 0; i < log->n_machines; i++)
        if (m->hash && !strcmp(log->hashes[i], m->hash)) return i;
    return -1;
}

void trace_describe(const SgModel *m, const TraceRec *r, char *out, size_t n) {
    const char *f = r->from < m->n_states ? m->states[r->from].id : "?";
    const char *e = r->event < m->events.n ? m->events.v[r->event] : "?";
    const char *t = r->to < m->n_states ? m->states[r->to].id : "?";
    snprintf(out, n, "%s --%s--> %s", f, e, t);
}

int trace_show(const char *path, const SgModel *const *models, int n_models) {
    SgTrace log;
    char err[512];
    if (trace_load(path, &log, err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    const SgModel *by_entry[32] = {0};
    int bad = 0;
    for (int i = 0; i < log.n_machines; i++) {
        for (int k = 0; k < n_models && !by_entry[i]; k++)
            if (models[k]->hash && !strcmp(models[k]->hash, log.hashes[i])) by_entry[i] = models[k];
        if (!by_entry[i]) {
            fprintf(stderr, "%s: machine %s was recorded from a model that is not here (hash %.12s...); "
                            "regenerate and record again\n", path, log.ids[i], log.hashes[i]);
            bad = 1;
        }
    }
    if (bad) { free(log.recs); return 1; }
    for (int i = 0; i < log.n; i++) {
        const TraceRec *r = &log.recs[i];
        if (r->machine >= log.n_machines) { printf("%8u ms  #%-5u ? machine %u\n", r->t_ms, r->seq, r->machine); continue; }
        char d[256];
        trace_describe(by_entry[r->machine], r, d, sizeof d);
        printf("%8u ms  #%-5u %s[%u]  %s\n", r->t_ms, r->seq, log.ids[r->machine], r->instance, d);
    }
    printf("%d transitions from %d machine%s\n", log.n, log.n_machines, log.n_machines == 1 ? "" : "s");
    free(log.recs);
    return 0;
}
