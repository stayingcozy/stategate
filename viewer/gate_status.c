#include "gate_status.h"

#include <stdlib.h>
#include <string.h>

#include "ydoc.h"

static void sl_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

void gate_status_free(GateStatus *g) {
    free(g->time);
    free(g->ticket);
    free(g->ticket_status);
    free(g->step);
    sl_free(&g->stubs);
    sl_free(&g->errors);
    for (int i = 0; i < g->n_checks; i++) {
        free(g->checks[i].name);
        free(g->checks[i].status);
        sl_free(&g->checks[i].messages);
    }
    memset(g, 0, sizeof *g);
}

/* JSON null arrives as the plain scalar "null". */
static char *str_or_null(YReader *r, yaml_node_t *n, const char *what) {
    char *s = y_str(r, n, what);
    if (s && !strcmp(s, "null")) { free(s); return NULL; }
    return s;
}

void gate_status_load(GateStatus *g, const char *path) {
    gate_status_free(g);
    if (!file_exists(path)) return;
    YReader r;
    if (y_load(&r, path, &g->errors) != 0) { y_free(&r); return; }
    yaml_node_t *root = y_root(&r);
    g->present = 1;
    g->ok = y_bool(&r, y_get(&r, root, "ok"), "ok", 0);
    g->release = y_bool(&r, y_get(&r, root, "release"), "release", 0);
    g->time = str_or_null(&r, y_get(&r, root, "time"), "time");
    g->ticket = str_or_null(&r, y_get(&r, root, "ticket"), "ticket");
    g->ticket_status = str_or_null(&r, y_get(&r, root, "ticket_status"), "ticket_status");
    g->step = str_or_null(&r, y_get(&r, root, "step"), "step");
    g->failures = (int)y_int(&r, y_get(&r, root, "failures"), "failures", 0);
    g->max_failures = (int)y_int(&r, y_get(&r, root, "max_failures"), "max_failures", 3);
    g->stub_count = (int)y_int(&r, y_get(&r, root, "stub_count"), "stub_count", 0);
    y_strlist(&r, y_get(&r, root, "stubs"), "stubs", &g->stubs);
    yaml_node_t *checks = y_get(&r, root, "checks");
    if (y_is_seq(checks)) {
        for (yaml_node_item_t *it = checks->data.sequence.items.start;
             it < checks->data.sequence.items.top && g->n_checks < GATE_MAX_CHECKS; it++) {
            yaml_node_t *c = y_node(&r, *it);
            GateCheck *gc = &g->checks[g->n_checks++];
            gc->name = y_str(&r, y_get(&r, c, "name"), "name");
            gc->status = y_str(&r, y_get(&r, c, "status"), "status");
            if (!gc->name) gc->name = xstrdup("?");
            if (!gc->status) gc->status = xstrdup("skip");
            y_strlist(&r, y_get(&r, c, "messages"), "messages", &gc->messages);
        }
    }
    y_free(&r);
}
