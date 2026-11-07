#include "medit.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "util.h"

/* ---- the parsed text ---- */

typedef struct {
    yaml_document_t doc;
    const char *t;
    size_t *cb; /* libyaml marks count characters: character index -> byte offset */
    size_t nchars;
} D;

static int d_load(D *d, const char *t) {
    memset(d, 0, sizeof *d);
    d->t = t;
    size_t len = strlen(t), n = 0;
    d->cb = xcalloc(len + 2, sizeof(size_t));
    for (size_t i = 0; i < len; i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) d->cb[n++] = i;
    d->cb[n] = len;
    d->nchars = n;
    yaml_parser_t p;
    yaml_parser_initialize(&p);
    yaml_parser_set_input_string(&p, (const unsigned char *)t, len);
    int ok = yaml_parser_load(&p, &d->doc);
    yaml_parser_delete(&p);
    if (!ok) { free(d->cb); return -1; }
    if (!yaml_document_get_root_node(&d->doc)) {
        yaml_document_delete(&d->doc);
        free(d->cb);
        return -1;
    }
    return 0;
}

static void d_free(D *d) {
    yaml_document_delete(&d->doc);
    free(d->cb);
}

static size_t B(const D *d, yaml_mark_t m) { return d->cb[m.index <= d->nchars ? m.index : d->nchars]; }
static yaml_node_t *N(D *d, int i) { return yaml_document_get_node(&d->doc, i); }
static int is_map(const yaml_node_t *n) { return n && n->type == YAML_MAPPING_NODE; }
static int is_seq(const yaml_node_t *n) { return n && n->type == YAML_SEQUENCE_NODE; }
static int is_scalar(const yaml_node_t *n) { return n && n->type == YAML_SCALAR_NODE; }
static const char *S(const yaml_node_t *n) { return is_scalar(n) ? (const char *)n->data.scalar.value : ""; }
static int is_flow(const yaml_node_t *n) {
    return (is_seq(n) && n->data.sequence.style == YAML_FLOW_SEQUENCE_STYLE) ||
           (is_map(n) && n->data.mapping.style == YAML_FLOW_MAPPING_STYLE);
}
static int n_items(const yaml_node_t *s) { return is_seq(s) ? (int)(s->data.sequence.items.top - s->data.sequence.items.start) : 0; }
static int n_pairs(const yaml_node_t *m) { return is_map(m) ? (int)(m->data.mapping.pairs.top - m->data.mapping.pairs.start) : 0; }
static yaml_node_t *item(D *d, yaml_node_t *s, int i) { return N(d, s->data.sequence.items.start[i]); }
static yaml_node_t *pkey(D *d, yaml_node_t *m, int i) { return N(d, m->data.mapping.pairs.start[i].key); }
static yaml_node_t *pval(D *d, yaml_node_t *m, int i) { return N(d, m->data.mapping.pairs.start[i].value); }

static int find_key(D *d, yaml_node_t *m, const char *key) {
    for (int i = 0; i < n_pairs(m); i++)
        if (!strcmp(S(pkey(d, m, i)), key)) return i;
    return -1;
}

static yaml_node_t *get(D *d, yaml_node_t *m, const char *key) {
    int i = find_key(d, m, key);
    return i < 0 ? NULL : pval(d, m, i);
}

static int seq_find(D *d, yaml_node_t *s, const char *v) {
    for (int i = 0; i < n_items(s); i++)
        if (!strcmp(S(item(d, s, i)), v)) return i;
    return -1;
}

static yaml_node_t *root(D *d) { return yaml_document_get_root_node(&d->doc); }
static yaml_node_t *top(D *d, const char *key) { return get(d, root(d), key); }

static size_t nstart(D *d, yaml_node_t *n) { return B(d, n->start_mark); }

/* Where a node's own text ends: block collections end with their last
 * child, not where libyaml's next token starts. */
static size_t nend(D *d, yaml_node_t *n) {
    if (is_scalar(n) || is_flow(n)) return B(d, n->end_mark);
    if (is_seq(n) && n_items(n)) return nend(d, item(d, n, n_items(n) - 1));
    if (is_map(n) && n_pairs(n)) return nend(d, pval(d, n, n_pairs(n) - 1));
    return B(d, n->end_mark);
}

static yaml_node_t *state_node(D *d, const char *id) {
    yaml_node_t *s = top(d, "states");
    for (int i = 0; i < n_items(s); i++) {
        yaml_node_t *m = item(d, s, i);
        if (is_map(m) && !strcmp(S(get(d, m, "id")), id)) return m;
    }
    return NULL;
}

/* The display name: `name`, else the id after its last dot. */
static const char *state_name(D *d, yaml_node_t *m) {
    yaml_node_t *n = get(d, m, "name");
    if (n) return S(n);
    const char *id = S(get(d, m, "id")), *dot = strrchr(id, '.');
    return dot ? dot + 1 : id;
}

static void state_ids(D *d, StrList *out) {
    yaml_node_t *s = top(d, "states");
    for (int i = 0; i < n_items(s); i++) {
        yaml_node_t *m = item(d, s, i);
        if (is_map(m)) sl_push(out, S(get(d, m, "id")));
    }
}

static void sl_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* ---- text ---- */

static size_t lstart(const char *t, size_t o) {
    while (o > 0 && t[o - 1] != '\n') o--;
    return o;
}

/* Just past the end of the line holding o (its comment and newline too). */
static size_t lnext(const char *t, size_t o) {
    while (t[o] && t[o] != '\n') o++;
    return t[o] == '\n' ? o + 1 : o;
}

static int only_spaces(const char *t, size_t a, size_t b) {
    for (; a < b; a++)
        if (t[a] != ' ' && t[a] != '\t') return 0;
    return 1;
}

static int blank_line_between(const char *t, size_t a, size_t b) {
    for (size_t o = a; o < b; o = lnext(t, o)) {
        size_t e = o;
        while (t[e] && t[e] != '\n') e++;
        if (only_spaces(t, o, e)) return 1;
        if (e == o) break;
    }
    return 0;
}

static size_t column(const char *t, size_t o) { return o - lstart(t, o); }

/* One splice: replace [a, b) with s. */
typedef struct {
    size_t a, b;
    SB s;
    int set;
} Ed;

static void ed(Ed *e, size_t a, size_t b) {
    e->a = a;
    e->b = b;
    sb_init(&e->s);
    e->set = 1;
}

static void ed_apply(char **t, Ed *e) {
    if (!e->set) return;
    size_t len = strlen(*t), n = e->s.len;
    char *o = xcalloc(len - (e->b - e->a) + n + 1, 1);
    memcpy(o, *t, e->a);
    memcpy(o + e->a, e->s.s, n);
    memcpy(o + e->a + n, *t + e->b, len - e->b);
    free(*t);
    *t = o;
    free(e->s.s);
    e->set = 0;
}

static void indent(SB *b, size_t n) {
    for (size_t i = 0; i < n; i++) sb_puts(b, " ");
}

/* ---- primitive splices ---- */

static void p_replace(D *d, yaml_node_t *n, const char *s, Ed *e) {
    ed(e, nstart(d, n), nend(d, n));
    sb_puts(&e->s, s);
}

static void p_map_remove(D *d, yaml_node_t *m, int i, Ed *e) {
    int n = n_pairs(m);
    const char *t = d->t;
    if (is_flow(m)) {
        if (n == 1) { ed(e, nstart(d, m), nend(d, m)); sb_puts(&e->s, "{}"); }
        else if (i < n - 1) ed(e, nstart(d, pkey(d, m, i)), nstart(d, pkey(d, m, i + 1)));
        else ed(e, nend(d, pval(d, m, i - 1)), nend(d, pval(d, m, i)));
        return;
    }
    size_t ks = nstart(d, pkey(d, m, i)), ls = lstart(t, ks);
    if (!only_spaces(t, ls, ks)) /* the first key, on the "- " line */
        ed(e, ks, i + 1 < n ? nstart(d, pkey(d, m, i + 1)) : nend(d, pval(d, m, i)));
    else
        ed(e, ls, lnext(t, nend(d, pval(d, m, i))));
}

/* Adds `key: value`. In a block mapping a `block` value goes on its own
 * line under the key, else `flow` follows the colon; the pair goes before
 * the first key of `before` the mapping has, or last. */
static void p_map_add(D *d, yaml_node_t *m, const char *key, const char *flow, const char *block,
                      const char *const *before, Ed *e) {
    const char *t = d->t;
    int n = n_pairs(m);
    if (is_flow(m) || !n) {
        if (!n) {
            ed(e, nstart(d, m), nend(d, m));
            sb_printf(&e->s, "{ %s: %s }", key, flow);
        } else {
            size_t at = nend(d, pval(d, m, n - 1));
            ed(e, at, at);
            sb_printf(&e->s, ", %s: %s", key, flow);
        }
        return;
    }
    size_t col = column(t, nstart(d, pkey(d, m, 0)));
    size_t at = (size_t)-1;
    for (int i = 0; i < n && at == (size_t)-1 && before; i++) {
        const char *k = S(pkey(d, m, i));
        for (const char *const *b = before; *b; b++) {
            if (strcmp(*b, k)) continue;
            size_t ks = nstart(d, pkey(d, m, i));
            if (only_spaces(t, lstart(t, ks), ks)) at = lstart(t, ks);
            break;
        }
    }
    if (at == (size_t)-1) at = lnext(t, nend(d, m));
    ed(e, at, at);
    if (at > 0 && t[at - 1] != '\n') sb_puts(&e->s, "\n");
    indent(&e->s, col);
    if (block) {
        sb_printf(&e->s, "%s:\n", key);
        indent(&e->s, col + 2);
        sb_printf(&e->s, "%s\n", block);
    } else {
        sb_printf(&e->s, "%s: %s\n", key, flow);
    }
}

static void p_seq_remove(D *d, yaml_node_t *s, int i, Ed *e) {
    int n = n_items(s);
    const char *t = d->t;
    if (is_flow(s)) {
        if (n == 1) { ed(e, nstart(d, s), nend(d, s)); sb_puts(&e->s, "[]"); }
        else if (i < n - 1) ed(e, nstart(d, item(d, s, i)), nstart(d, item(d, s, i + 1)));
        else ed(e, nend(d, item(d, s, i - 1)), nend(d, item(d, s, i)));
        return;
    }
    /* Whole lines, with the blank lines that separated this item. */
    if (i < n - 1) ed(e, lstart(t, nstart(d, item(d, s, i))), lstart(t, nstart(d, item(d, s, i + 1))));
    else if (i > 0) ed(e, lnext(t, nend(d, item(d, s, i - 1))), lnext(t, nend(d, item(d, s, i))));
    else ed(e, lstart(t, nstart(d, item(d, s, i))), lnext(t, nend(d, item(d, s, i))));
}

/* Appends an item: `flow` in a flow sequence, else `block` (lines joined
 * by \n) after a "- ", spaced like the items already there. */
static void p_seq_append(D *d, yaml_node_t *s, const char *flow, const char *block, Ed *e) {
    const char *t = d->t;
    int n = n_items(s);
    if (!n) {
        if (is_flow(s) && !strchr(block, '\n')) {
            ed(e, nstart(d, s), nend(d, s));
            sb_printf(&e->s, "[%s]", flow);
            return;
        }
        /* `key: []` becomes a block list under the key. */
        size_t a = nstart(d, s);
        while (a > 0 && t[a - 1] == ' ') a--;
        size_t ls = lstart(t, a), keycol = 0;
        while (ls + keycol < a && t[ls + keycol] == ' ') keycol++;
        ed(e, a, nend(d, s));
        sb_puts(&e->s, "\n");
        const char *p = block;
        int first = 1;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            indent(&e->s, keycol + 2 + (first ? 0 : 2));
            if (first) sb_puts(&e->s, "- ");
            sb_printf(&e->s, "%.*s", (int)len, p);
            if (nl) sb_puts(&e->s, "\n");
            first = 0;
            p += len + (nl ? 1 : 0);
        }
        return;
    }
    if (is_flow(s)) {
        size_t at = nend(d, item(d, s, n - 1));
        ed(e, at, at);
        sb_printf(&e->s, ", %s", flow);
        return;
    }
    size_t dash = column(t, nstart(d, s)), col = column(t, nstart(d, item(d, s, 0)));
    int spaced = n >= 2 && blank_line_between(t, lnext(t, nend(d, item(d, s, 0))), lstart(t, nstart(d, item(d, s, 1))));
    size_t at = lnext(t, nend(d, item(d, s, n - 1)));
    ed(e, at, at);
    if (at > 0 && t[at - 1] != '\n') sb_puts(&e->s, "\n");
    if (spaced) sb_puts(&e->s, "\n");
    const char *p = block;
    int first = 1;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (first) { indent(&e->s, dash); sb_puts(&e->s, "- "); for (size_t k = dash + 2; k < col; k++) sb_puts(&e->s, " "); }
        else indent(&e->s, col);
        sb_printf(&e->s, "%.*s\n", (int)len, p);
        first = 0;
        p += len + (nl ? 1 : 0);
    }
}

/* ---- helpers over the text ---- */

static int fail(char *err, size_t n, const char *fmt, ...) SG_PRINTF(3, 4);
static int fail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

#define LOAD(d)                                                                     \
    do {                                                                            \
        if (d_load(&(d), *t) != 0) return fail(err, errn, "The model is not valid YAML, so it can't be edited here: fix it in your editor."); \
    } while (0)

static const char *const BEFORE_INITIAL[] = {"entry", "exit", "invoke", "after", "on", "ignore", "layout", "note", NULL};
static const char *const BEFORE_ON[] = {"ignore", "layout", "note", NULL};
static const char *const BEFORE_IGNORE[] = {"layout", "note", NULL};
static const char *const BEFORE_LAYOUT[] = {"note", NULL};

static void num(char *out, size_t n, double v) {
    if (v == (double)(long)v) snprintf(out, n, "%ld", (long)v);
    else snprintf(out, n, "%g", v);
}

static const char *on_target(D *d, yaml_node_t *st, const char *ev) {
    yaml_node_t *v = get(d, get(d, st, "on"), ev);
    if (is_map(v)) return S(get(d, v, "target"));
    return v ? S(v) : NULL;
}

/* Adds v to a state's list `key` (ignore), creating the list. */
static int list_add(char **t, const char *id, const char *key, const char *v, const char *const *before, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *st = state_node(&d, id), *l = st ? get(&d, st, key) : NULL;
    Ed e = {0};
    if (!st) { d_free(&d); return fail(err, errn, "No state %s.", id); }
    if (is_seq(l)) {
        if (seq_find(&d, l, v) < 0) p_seq_append(&d, l, v, v, &e);
    } else if (!l) {
        char flow[256];
        snprintf(flow, sizeof flow, "[%s]", v);
        p_map_add(&d, st, key, flow, NULL, before, &e);
    } else {
        d_free(&d);
        return fail(err, errn, "%s: '%s' is not a list.", id, key);
    }
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

/* Removes v from a list under `owner` (a state id, or NULL for the top
 * level); a list left empty goes, key and all. */
static int list_remove(char **t, const char *owner, const char *key, const char *v, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *m = owner ? state_node(&d, owner) : root(&d), *l = m ? get(&d, m, key) : NULL;
    int i = is_seq(l) ? seq_find(&d, l, v) : -1;
    Ed e = {0};
    if (i >= 0) {
        if (n_items(l) == 1) p_map_remove(&d, m, find_key(&d, m, key), &e);
        else p_seq_remove(&d, l, i, &e);
    }
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

/* An event nothing uses any more (no transition, timer, service or
 * invariant names it) leaves `events` and every `ignore`. */
static int prune_event(char **t, const char *ev, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *events = top(&d, "events");
    int nn = (int)(d.doc.nodes.top - d.doc.nodes.start);
    char *skip = xcalloc((size_t)nn + 1, 1);
    for (int i = 0; i < n_items(events); i++) skip[events->data.sequence.items.start[i]] = 1;
    int used = 0;
    for (int i = 1; i <= nn && !used; i++)
        if (!skip[i] && is_scalar(N(&d, i)) && !strcmp(S(N(&d, i)), ev)) used = 1;
    free(skip);
    int declared = seq_find(&d, events, ev) >= 0, last = n_items(events) <= 1;
    StrList ids = {0};
    state_ids(&d, &ids);
    d_free(&d);
    if (used || !declared || last) { sl_free(&ids); return 0; }
    int rc = list_remove(t, NULL, "events", ev, err, errn);
    for (int i = 0; rc == 0 && i < ids.n; i++) rc = list_remove(t, ids.v[i], "ignore", ev, err, errn);
    sl_free(&ids);
    return rc;
}

/* ---- names ---- */

int me_is_event_name(const char *s) {
    if (!s || !*s || !isupper((unsigned char)*s)) return 0;
    for (; *s; s++)
        if (!(isupper((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

int me_is_state_name(const char *s) {
    if (!s || !*s || !isupper((unsigned char)*s)) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s)) return 0;
    return 1;
}

/* ---- edits ---- */

int me_add_state(char **t, const char *name, double x, double y, char *err, size_t errn) {
    if (!me_is_state_name(name)) return fail(err, errn, "State names are CamelCase letters and digits, like Idle or Running2.");
    D d;
    LOAD(d);
    const char *mid = S(get(&d, top(&d, "machine"), "id"));
    yaml_node_t *states = top(&d, "states");
    if (!*mid) { d_free(&d); return fail(err, errn, "The model has no machine id."); }
    if (!is_seq(states)) { d_free(&d); return fail(err, errn, "The model has no 'states' list."); }
    char id[300];
    snprintf(id, sizeof id, "%s.%s", mid, name);
    for (int i = 0; i < n_items(states); i++) {
        yaml_node_t *m = item(&d, states, i);
        if (!is_map(m)) continue;
        if (!strcmp(S(get(&d, m, "id")), id) || !strcmp(state_name(&d, m), name)) {
            d_free(&d);
            return fail(err, errn, "There is already a state called %s.", name);
        }
    }
    char xs[32], ys[32];
    num(xs, sizeof xs, x);
    num(ys, sizeof ys, y);
    SB block, flow;
    sb_init(&block);
    sb_init(&flow);
    sb_printf(&block, "id: %s\nname: %s\n", id, name);
    sb_printf(&flow, "{ id: %s, name: %s", id, name);
    if (!n_items(states)) { sb_puts(&block, "initial: true\n"); sb_puts(&flow, ", initial: true"); }
    sb_printf(&block, "layout: { x: %s, y: %s }", xs, ys);
    sb_printf(&flow, ", layout: { x: %s, y: %s } }", xs, ys);
    Ed e = {0};
    p_seq_append(&d, states, flow.s, block.s, &e);
    d_free(&d);
    ed_apply(t, &e);
    free(block.s);
    free(flow.s);
    return 0;
}

int me_rename_state(char **t, const char *id, const char *new_name, char *err, size_t errn) {
    if (!me_is_state_name(new_name)) return fail(err, errn, "State names are CamelCase letters and digits, like Idle or Running2.");
    D d;
    LOAD(d);
    yaml_node_t *st = state_node(&d, id), *states = top(&d, "states");
    if (!st) { d_free(&d); return fail(err, errn, "No state %s.", id); }
    const char *mid = S(get(&d, top(&d, "machine"), "id"));
    char nid[300];
    snprintf(nid, sizeof nid, "%s.%s", mid, new_name);
    for (int i = 0; i < n_items(states); i++) {
        yaml_node_t *m = item(&d, states, i);
        if (m == st || !is_map(m)) continue;
        if (!strcmp(S(get(&d, m, "id")), nid) || !strcmp(state_name(&d, m), new_name)) {
            d_free(&d);
            return fail(err, errn, "There is already a state called %s.", new_name);
        }
    }
    int has_name = get(&d, st, "name") != NULL;
    d_free(&d);
    if (!strcmp(id, nid)) goto name;
    /* Every scalar that is the old id: the state's own, transition
     * targets, invariants. */
    for (int guard = 0; guard < 10000; guard++) {
        LOAD(d);
        yaml_node_t *hit = NULL;
        int nn = (int)(d.doc.nodes.top - d.doc.nodes.start);
        for (int i = 1; i <= nn && !hit; i++)
            if (is_scalar(N(&d, i)) && !strcmp(S(N(&d, i)), id)) hit = N(&d, i);
        Ed e = {0};
        if (hit) p_replace(&d, hit, nid, &e);
        d_free(&d);
        if (!hit) break;
        ed_apply(t, &e);
    }
name:
    if (has_name) {
        LOAD(d);
        Ed e = {0};
        st = state_node(&d, nid);
        if (st) p_replace(&d, get(&d, st, "name"), new_name, &e);
        d_free(&d);
        ed_apply(t, &e);
    }
    return 0;
}

int me_set_initial(char **t, const char *id, char *err, size_t errn) {
    D d;
    LOAD(d);
    if (!state_node(&d, id)) { d_free(&d); return fail(err, errn, "No state %s.", id); }
    StrList others = {0};
    yaml_node_t *states = top(&d, "states");
    for (int i = 0; i < n_items(states); i++) {
        yaml_node_t *m = item(&d, states, i);
        const char *sid = S(get(&d, m, "id"));
        if (strcmp(sid, id) && get(&d, m, "initial")) sl_push(&others, sid);
    }
    d_free(&d);
    for (int i = 0; i < others.n; i++) {
        LOAD(d);
        yaml_node_t *m = state_node(&d, others.v[i]);
        Ed e = {0};
        p_map_remove(&d, m, find_key(&d, m, "initial"), &e);
        d_free(&d);
        ed_apply(t, &e);
    }
    sl_free(&others);
    LOAD(d);
    yaml_node_t *m = state_node(&d, id), *v = get(&d, m, "initial");
    Ed e = {0};
    if (v) { if (strcmp(S(v), "true")) p_replace(&d, v, "true", &e); }
    else p_map_add(&d, m, "initial", "true", NULL, BEFORE_INITIAL, &e);
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

static int set_layout_key(char **t, const char *id, const char *key, const char *v, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *l = get(&d, state_node(&d, id), "layout"), *x = get(&d, l, key);
    Ed e = {0};
    if (x) { if (strcmp(S(x), v)) p_replace(&d, x, v, &e); }
    else p_map_add(&d, l, key, v, NULL, NULL, &e);
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

int me_set_layout(char **t, const char *id, double x, double y, char *err, size_t errn) {
    char xs[32], ys[32];
    num(xs, sizeof xs, x);
    num(ys, sizeof ys, y);
    D d;
    LOAD(d);
    yaml_node_t *st = state_node(&d, id);
    if (!st) { d_free(&d); return fail(err, errn, "No state %s.", id); }
    yaml_node_t *l = get(&d, st, "layout");
    if (l && !is_map(l)) { d_free(&d); return fail(err, errn, "%s: 'layout' is not { x, y }.", id); }
    if (!l) {
        char flow[80];
        snprintf(flow, sizeof flow, "{ x: %s, y: %s }", xs, ys);
        Ed e = {0};
        p_map_add(&d, st, "layout", flow, NULL, BEFORE_LAYOUT, &e);
        d_free(&d);
        ed_apply(t, &e);
        return 0;
    }
    d_free(&d);
    if (set_layout_key(t, id, "x", xs, err, errn)) return -1;
    return set_layout_key(t, id, "y", ys, err, errn);
}

int me_set_transition(char **t, const char *from, const char *ev, const char *to, char *err, size_t errn) {
    if (!me_is_event_name(ev)) return fail(err, errn, "Event names are UPPER_SNAKE_CASE, like COIN or TIMER_DONE.");
    D d;
    LOAD(d);
    if (!state_node(&d, from)) { d_free(&d); return fail(err, errn, "No state %s.", from); }
    if (!state_node(&d, to)) { d_free(&d); return fail(err, errn, "No state %s.", to); }
    yaml_node_t *events = top(&d, "events");
    if (!is_seq(events)) { d_free(&d); return fail(err, errn, "The model has no 'events' list."); }
    int declared = seq_find(&d, events, ev) >= 0;
    StrList ids = {0};
    state_ids(&d, &ids);
    Ed e = {0};
    if (!declared) p_seq_append(&d, events, ev, ev, &e);
    d_free(&d);
    ed_apply(t, &e);
    /* Other states are left undecided about a new event: the canvas flags
     * them until each one handles or ignores it. */
    sl_free(&ids);
    if (list_remove(t, from, "ignore", ev, err, errn)) return -1;
    LOAD(d);
    yaml_node_t *st = state_node(&d, from), *on = get(&d, st, "on");
    if (on && !is_map(on)) { d_free(&d); return fail(err, errn, "%s: 'on' is not a mapping.", from); }
    if (on) {
        yaml_node_t *v = get(&d, on, ev);
        if (is_map(v)) v = get(&d, v, "target");
        if (v) { if (strcmp(S(v), to)) p_replace(&d, v, to, &e); }
        else p_map_add(&d, on, ev, to, NULL, NULL, &e);
    } else {
        char flow[300], block[300];
        snprintf(flow, sizeof flow, "{ %s: %s }", ev, to);
        snprintf(block, sizeof block, "%s: %s", ev, to);
        p_map_add(&d, st, "on", flow, block, BEFORE_ON, &e);
    }
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

/* Drops the pair from `on` (and `on` with it when it was the last), with
 * no other bookkeeping. */
static int drop_transition(char **t, const char *from, const char *ev, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *st = state_node(&d, from), *on = st ? get(&d, st, "on") : NULL;
    int k = find_key(&d, on, ev);
    if (k < 0) { d_free(&d); return fail(err, errn, "%s does not handle %s.", from, ev); }
    Ed e = {0};
    if (n_pairs(on) == 1) p_map_remove(&d, st, find_key(&d, st, "on"), &e);
    else p_map_remove(&d, on, k, &e);
    d_free(&d);
    ed_apply(t, &e);
    return 0;
}

int me_remove_transition(char **t, const char *from, const char *ev, char *err, size_t errn) {
    if (drop_transition(t, from, ev, err, errn)) return -1;
    return prune_event(t, ev, err, errn);
}

int me_ignore_event(char **t, const char *id, const char *ev, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *st = state_node(&d, id);
    int handled = st && find_key(&d, get(&d, st, "on"), ev) >= 0, declared = seq_find(&d, top(&d, "events"), ev) >= 0;
    d_free(&d);
    if (!st) return fail(err, errn, "No state %s.", id);
    if (!declared) return fail(err, errn, "%s is not a declared event.", ev);
    if (handled) return fail(err, errn, "%s already handles %s.", id, ev);
    return list_add(t, id, "ignore", ev, BEFORE_IGNORE, err, errn);
}

int me_delete_state(char **t, const char *id, char *err, size_t errn) {
    D d;
    LOAD(d);
    yaml_node_t *states = top(&d, "states"), *st = state_node(&d, id);
    if (!st) { d_free(&d); return fail(err, errn, "No state %s.", id); }
    if (n_items(states) <= 1) { d_free(&d); return fail(err, errn, "A machine needs at least one state."); }
    int was_initial = !strcmp(S(get(&d, st, "initial")), "true");
    StrList in_from = {0}, in_ev = {0}, touched = {0};
    for (int i = 0; i < n_items(states); i++) {
        yaml_node_t *m = item(&d, states, i), *on = get(&d, m, "on");
        int self = m == st;
        for (int k = 0; k < n_pairs(on); k++) {
            const char *ev = S(pkey(&d, on, k));
            if (self) { if (!sl_contains(&touched, ev)) sl_push(&touched, ev); continue; }
            const char *tg = on_target(&d, m, ev);
            if (tg && !strcmp(tg, id)) {
                sl_push(&in_from, S(get(&d, m, "id")));
                sl_push(&in_ev, ev);
                if (!sl_contains(&touched, ev)) sl_push(&touched, ev);
            }
        }
    }
    d_free(&d);
    int rc = 0;
    for (int i = 0; rc == 0 && i < in_from.n; i++) {
        rc = drop_transition(t, in_from.v[i], in_ev.v[i], err, errn);
    }
    if (!rc) {
        if (d_load(&d, *t) != 0) rc = fail(err, errn, "The model stopped parsing.");
        else {
            yaml_node_t *s2 = top(&d, "states");
            int idx = -1;
            for (int i = 0; i < n_items(s2); i++)
                if (!strcmp(S(get(&d, item(&d, s2, i), "id")), id)) idx = i;
            Ed e = {0};
            if (idx >= 0) p_seq_remove(&d, s2, idx, &e);
            d_free(&d);
            ed_apply(t, &e);
        }
    }
    if (!rc && was_initial) {
        StrList ids = {0};
        if (d_load(&d, *t) == 0) { state_ids(&d, &ids); d_free(&d); }
        if (ids.n) rc = me_set_initial(t, ids.v[0], err, errn);
        sl_free(&ids);
    }
    for (int i = 0; rc == 0 && i < touched.n; i++) rc = prune_event(t, touched.v[i], err, errn);
    sl_free(&in_from);
    sl_free(&in_ev);
    sl_free(&touched);
    return rc;
}

char *me_new_model(const char *mid) {
    SB b;
    sb_init(&b);
    sb_printf(&b,
              "# stategate model: %s.\n"
              "# Every (state, event) pair is either handled under `on` or listed under `ignore`.\n"
              "# Edit it here or on the stategate-view canvas: the app writes its edits back to this file.\n"
              "\n"
              "machine: { id: %s, version: 1, language: rust }\n"
              "\n"
              "events: [TICK]\n"
              "\n"
              "states:\n"
              "  - id: %s.Idle\n"
              "    name: Idle\n"
              "    initial: true                     # the black dot: the machine starts here\n"
              "    on:\n"
              "      TICK: %s.Idle\n"
              "    layout: { x: 80, y: 120 }\n"
              "\n"
              "steps:\n"
              "  - { id: step0, goal: \"First step: say what the machine should do\", status: active }\n",
              mid, mid, mid, mid);
    return b.s;
}
